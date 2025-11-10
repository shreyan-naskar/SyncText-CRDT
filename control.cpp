#include "headers.cpp"
#include "crdtUtils.cpp"

// MAIN
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <uid>\n";
        return 1;
    }

    gUID = argv[1];
    if (gUID.size() >= uid_LEN)
    {
        std::cerr << "uid too long\n";
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    ShmRegistry *reg = openReg();
    if (!reg)
    {
        std::cerr << "Failed to open shared registry\n";
        return 1;
    }
    gReg = reg;

    bool any_active = false;
    for (size_t i = 0; i < MAX_USERS; ++i)
    {
        if (reg->users[i].active != 0)
        {
            any_active = true;
            break;
        }
    }

    if (!any_active)
    {
        memset(reg, 0, sizeof(ShmRegistry));
        reg->numUsers = 0;
    }

    int slot = regUser(reg, gUID);
    if (slot == -1)
    {
        std::cerr << "[" << gUID << "] Registry full\n";
        munmap(reg, sizeof(ShmRegistry));
        return 1;
    }
    gMySlot = slot;

    size_t sys_max = maxSysMsgSize();
    gMQ_msgsize = (sys_max > 0) ? std::min<size_t>(sys_max, 8192) : 8192;

    gQName = string("/mq_") + gUID;
    if (!createSelfQ(gQName))
    {
        std::cerr << "[" << gUID << "] Failed to create own queue\n";
        deregSlot(reg, slot);
        munmap(reg, sizeof(ShmRegistry));
        return 1;
    }

    std::strncpy(reg->users[slot].qName, gQName.c_str(), NAME_QLEN - 1);
    std::cerr << "[" << gUID << "] Registered slot " << slot << ", queue " << gQName << "\n";

    string user_doc = gUID + "_doc.txt";
    verifyLocalDoc(user_doc);

    vector<string> doc_lines = readLinesFile(user_doc);
    vector<string> observed_lines = doc_lines;

    gLastDispLines = observed_lines;
    gPrevEdits.clear();

    dispDocUpdatesSimp(user_doc, observed_lines, reg);

    std::thread listener(listenerThreadFunc, gQName);

    vector<Update> outgoing_bufferfer;
    outgoing_bufferfer.reserve(32);

    vector<Update> local_unmerged;
    vector<Update> recv_unmerged;

    struct stat fst;
    if (stat(user_doc.c_str(), &fst) != 0)
        perror("stat initial");
    time_t last_mtime = fst.st_mtime;

    while (!gExit.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(POLL_INTERVAL_SEC));

        struct stat st;
        if (stat(user_doc.c_str(), &st) == 0)
        {
            if (st.st_mtime != last_mtime)
            {
                vector<string> new_lines = readLinesFile(user_doc);
                vector<Update> updates = diffLinesMakeUpdates(observed_lines, new_lines, gUID);

                observed_lines.swap(new_lines);
                last_mtime = st.st_mtime;

                if (!updates.empty())
                {
                    std::cerr << "[" << gUID << "] Detected " << updates.size() << " local update(s)\n";
                    for (auto &u : updates)
                    {
                        local_unmerged.push_back(u);
                        outgoing_bufferfer.push_back(u);
                    }
                    gPrevEdits = updates;
                    g_recent_notifications.clear();
                    g_show_merge_message = false;
                }
                else
                {
                    gPrevEdits.clear();
                }

                dispDocUpdatesSimp(user_doc, observed_lines, reg);

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

                            string target_queue =
                                (reg->users[i].qName[0]) ?
                                string(reg->users[i].qName) :
                                (string("/mq_") + target);

                            sendRetriesUpdatesToQ(target_queue, s, 6, 100);
                        }
                    }
                    outgoing_bufferfer.clear();
                }
            }
        }

        string serialized;
        while (gRingRecv.pop(serialized))
        {
            Update temp;
            if (!updateDeserialize(serialized, temp))
            {
                std::cerr << "[" << gUID << "] WARN: failed to deserialize incoming update\n";
            }
            else
            {
                recv_unmerged.push_back(temp);
            }
        }

        int total_pending = (int)local_unmerged.size() + (int)recv_unmerged.size();
        if (total_pending > 0 && total_pending >= BROADCAST_BATCH_SIZE)
        {
            vector<Update> all;
            all.insert(all.end(), local_unmerged.begin(), local_unmerged.end());
            all.insert(all.end(), recv_unmerged.begin(), recv_unmerged.end());

            g_recent_notifications.clear();
            vector<Update> winners = crdtMerge(all);

            if (!winners.empty())
            {
                applyLineUpdates(doc_lines, winners);
                writeLinesFile(user_doc, doc_lines);

                observed_lines = doc_lines;

                struct stat st2;
                if (stat(user_doc.c_str(), &st2) == 0)
                    last_mtime = st2.st_mtime;

                gPrevEdits.clear();
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
                std::cerr << "[" << gUID << "] No winning updates after merge\n";
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
