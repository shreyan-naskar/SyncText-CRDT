#include "headers.cpp"
#include "crdtUtils.cpp"

// ---------------- MAIN ----------------
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        cerr << "Usage: " << argv[0] << " <uid>\n";
        return 1;
    }
    gUID = argv[1];
    if (gUID.size() >= uid_LEN)
    {
        cerr << "uid too long\n";
        return 1;
    }
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // prepare registry
    ShmRegistry *reg = openReg();
    if (!reg)
    {
        cerr << "Failed to open shared registry\n";
        return 1;
    }
    gReg = reg;

    // best-effort zero init if registry looks unused
    bool any_active = false;
    for (size_t i = 0; i < MAX_USERS; ++i)
        if (reg->users[i].active != 0)
        {
            any_active = true;
            break;
        }
    if (!any_active)
    {
        memset(reg, 0, sizeof(ShmRegistry));
        reg->numUsers = 0;
    }

    // register user slot
    int slot = regUser(reg, gUID);
    if (slot == -1)
    {
        cerr << "[" << gUID << "] Registry full\n";
        munmap(reg, sizeof(ShmRegistry));
        return 1;
    }
    gMySlot = slot;

    // decide safe mq msgsize
    size_t sys_max = maxSysMsgSize();
    if (sys_max > 0)
        gMQ_msgsize = min<size_t>(sys_max, 8192);
    else
        gMQ_msgsize = 8192;

    // create own queue
    gQName = string("/mq_") + gUID;
    if (!createSelfQ(gQName))
    {
        cerr << "[" << gUID << "] Failed to create own queue\n";
        deregSlot(reg, slot);
        munmap(reg, sizeof(ShmRegistry));
        return 1;
    }
    strncpy(reg->users[slot].qName, gQName.c_str(), NAME_QLEN - 1);
    cerr << "[" << gUID << "] Registered slot " << slot << ", queue " << gQName << "\n";

    // ensure local doc exists
    string user_doc = gUID + string("_doc.txt");
    verifyLocalDoc(user_doc);

    // Authoritative merged state (what we last wrote) and last observed on disk
    vector<string> doc_lines = readLinesFile(user_doc); // authoritative
    vector<string> observed_lines = doc_lines;            // last seen on disk

    // initialize display bufferfers
    gLastDispLines = observed_lines;
    gPrevEdits.clear();

    // initial display
    dispDocUpdatesSimp(user_doc, observed_lines, reg);

    // start listener thread
    thread listener(listenerThreadFunc, gQName);

    // outgoing and pending bufferfers
    vector<Update> outgoing_bufferfer;
    outgoing_bufferfer.reserve(32);
    vector<Update> local_unmerged, recv_unmerged;

    // file mtime
    struct stat fst;
    if (stat(user_doc.c_str(), &fst) != 0)
        perror("stat initial");
    time_t last_mtime = fst.st_mtime;

    while (!gExit.load())
    {
        this_thread::sleep_for(chrono::seconds(POLL_INTERVAL_SEC));

        struct stat st;
        if (stat(user_doc.c_str(), &st) != 0)
        {
            continue;
        }
        if (st.st_mtime != last_mtime)
        {
            // Disk changed: read current file and diff against last observed (NOT against doc_lines)
            vector<string> new_lines = readLinesFile(user_doc);
            vector<Update> updates = diffLinesMakeUpdates(observed_lines, new_lines, gUID);

            observed_lines.swap(new_lines); // advance observation window
            last_mtime = st.st_mtime;

            if (!updates.empty())
            {
                cerr << "[" << gUID << "] Detected " << updates.size() << " local update(s)\n";
                for (auto &u : updates)
                {
                    local_unmerged.push_back(u);
                    outgoing_bufferfer.push_back(u);
                }
                // LOCAL edits → show deended diffs and reset notifications
                gPrevEdits = updates;
                g_recent_notifications.clear();
                g_show_merge_message = false;
            }
            else
            {
                gPrevEdits.clear();
            }

            // show what's actually on disk right now
            dispDocUpdatesSimp(user_doc, observed_lines, reg);

            // broadcast once batch ready
            if ((int)outgoing_bufferfer.size() >= BROADCAST_BATCH_SIZE)
            {
                for (const auto &u : outgoing_bufferfer)
                {
                    string s = serialize_update(u);
                    for (size_t i = 0; i < MAX_USERS; ++i)
                    {
                        if (__atomic_load_n(&reg->users[i].active, __ATOMIC_SEQ_CST) != 1)
                            continue;
                        string target = string(reg->users[i].uid);
                        if (target.empty() || target == gUID)
                            continue;
                        string target_queue = (reg->users[i].qName[0]) ? string(reg->users[i].qName) : (string("/mq_") + target);
                        sendRetriesUpdatesToQ(target_queue, s, 6, 100);
                    }
                }
                outgoing_bufferfer.clear();
            }
        }

        // drain incoming
        string serialized;
        while (gRingRecv.pop(serialized))
        {
            Update u;
            if (updateDeserialize(serialized, u))
            {
                recv_unmerged.push_back(u);
            }
            else
            {
                cerr << "[" << gUID << "] WARN: failed to deserialize incoming update\n";
            }
        }

        // merge when enough pending
        int total_pending = (int)local_unmerged.size() + (int)recv_unmerged.size();
        if (total_pending >= BROADCAST_BATCH_SIZE && total_pending > 0)
        {
            vector<Update> all;
            all.insert(all.end(), local_unmerged.begin(), local_unmerged.end());
            all.insert(all.end(), recv_unmerged.begin(), recv_unmerged.end());

            // Clear old notifications so only the latest merge is shown
            g_recent_notifications.clear();

            vector<Update> winners = crdtMerge(all);
            if (!winners.empty())
            {
                // Apply winners to authoritative doc_lines only, then write to disk
                applyLineUpdates(doc_lines, winners);
                writeLinesFile(user_doc, doc_lines);

                // After we write, align observation with authoritative state
                observed_lines = doc_lines;

                // refresh mtime after write to avoid re-diffing our own write
                struct stat st2;
                if (stat(user_doc.c_str(), &st2) == 0)
                    last_mtime = st2.st_mtime;

                // Remote merges should NOT show 'Change detected' lines
                gPrevEdits.clear();

                // Determine if a conflict happened (multiple updates targeting same line)
                bool conflict_detected = (all.size() > winners.size());

                if (conflict_detected)
                    g_recent_notifications.push_back("Conflict detected and resolved using LWW");
                else
                    g_recent_notifications.push_back("All updates merged successfully");

                g_show_merge_message = true;

                dispDocUpdatesSimp(user_doc, observed_lines, reg);
            }
            else
            {
                gPrevEdits.clear();
                cerr << "[" << gUID << "] No winning updates after merge\n";
            }

            local_unmerged.clear();
            recv_unmerged.clear();
        }
    }

    if (listener.joinable())
        listener.join();
    cleanExit(0);
    return 0;
}
