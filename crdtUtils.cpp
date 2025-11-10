#include "headers.cpp"
#include "display.cpp"

// LISTENER THREAD
void listenerThreadFunc(const string &qName)
{
    mqd_t mq = mq_open(qName.c_str(), O_RDONLY);
    if (mq == (mqd_t)-1)
    {
        perror(("listener mq_open " + qName).c_str());
        return;
    }

    struct mq_attr attr;
    (void)mq_getattr(mq, &attr);

    std::cerr << "[" << gUID << "] Listener running on " << qName << "\n";

    vector<char> buffer(gMQ_msgsize + 10);

    while (!gExit.load())
    {
        ssize_t n = mq_receive(mq, buffer.data(), buffer.size(), nullptr);

        if (n >= 0)
        {
            string s(buffer.data(), static_cast<size_t>(n));

            bool pushed = gRingRecv.push(s);
            if (!pushed)
            {
                std::cerr << "[" << gUID << "] WARN: recv ring full, dropping message\n";
            }
            else
            {
                Update tmp;
                if (updateDeserialize(s, tmp))
                {
                    g_recent_notifications.push_back(
                        "Received update from " + tmp.uid +
                        ": Line " + std::to_string(tmp.lineNum) + " modified");
                    g_show_merge_message = true;
                }
                else
                {
                    std::cerr << "[" << gUID << "] Received (badly formed) message\n";
                }
            }
        }
        else
        {
            if (errno == EINTR)
            {
                continue;
            }

            sleepMS(50);
        }
    }

    mq_close(mq);
}

// CRDT MERGE (LWW)
bool collisionUpdates(const Update &a, const Update &b)
{
    if (a.lineNum != b.lineNum)
    {
        return false;
    }

    int aStart = (a.startCol < a.endCol ? a.startCol : a.endCol);
    int aEnd   = (a.startCol > a.endCol ? a.startCol : a.endCol);

    int bStart = (b.startCol < b.endCol ? b.startCol : b.endCol);
    int bEnd   = (b.startCol > b.endCol ? b.startCol : b.endCol);

    int aLen = std::max(0, aEnd - aStart);
    int bLen = std::max(0, bEnd - bStart);

    if (aLen == 0 && bLen == 0)
    {
        // Both are insertion operations; collide if inserting at same position.
        return (aStart == bStart);
    }

    if (aLen == 0 && bLen > 0)
    {
        // a inserts inside b's replaced span
        return (aStart >= bStart && aStart < bEnd);
    }

    if (bLen == 0 && aLen > 0)
    {
        // b inserts inside a's replaced span
        return (bStart >= aStart && bStart < aEnd);
    }

    // Both are range edits: detect half-open interval overlap
    return (aStart < bEnd && bStart < aEnd);
}
bool updatesAonB(const Update &a, const Update &b)
{
    if (a.timestamp == b.timestamp)
    {
        return (a.uid < b.uid);
    }

    return (a.timestamp > b.timestamp);
}

vector<Update> crdtMerge(const vector<Update> &all)
{
    const size_t n = all.size();
    vector<bool> keep(n, true);

    for (size_t i = 0; i < n; ++i)
    {
        if (keep[i])
        {
            const Update &ui = all[i];

            for (size_t j = i + 1; j < n; ++j)
            {
                if (keep[j])
                {
                    const Update &uj = all[j];

                    if (collisionUpdates(ui, uj))
                    {
                        if (updatesAonB(ui, uj))
                        {
                            keep[j] = false;
                        }
                        else
                        {
                            keep[i] = false;
                        }
                    }
                }
            }
        }
    }

    vector<Update> out;
    out.reserve(n);
    for (size_t k = 0; k < n; ++k)
    {
        if (keep[k])
        {
            out.push_back(all[k]);
        }
    }
    return out;
}


void applyLineUpdates(vector<string> &lines, const vector<Update> &wins)
{
    for (const auto &u : wins)
    {
        if (u.lineNum >= 0)
        {
            if (static_cast<size_t>(u.lineNum) >= lines.size())
            {
                lines.resize(u.lineNum + 1);
            }

            string &line = lines[u.lineNum];

            if (u.toDo == "delete")
            {
                int start = std::max(0, u.startCol);
                int len = std::max(0, u.endCol - u.startCol);

                if (start < static_cast<int>(line.size()))
                {
                    line.erase(static_cast<size_t>(start), static_cast<size_t>(len));
                }
            }
            else if (u.toDo == "insert")
            {
                int pos = std::max(0, u.startCol);
                if (pos > static_cast<int>(line.size()))
                {
                    pos = static_cast<int>(line.size());
                }
                line.insert(static_cast<size_t>(pos), u.newContent);
            }
            else if (u.toDo == "replace")
            {
                int start = std::max(0, u.startCol);
                int len = std::max(0, u.endCol - u.startCol);

                if (start > static_cast<int>(line.size()))
                {
                    start = static_cast<int>(line.size());
                }

                line.erase(static_cast<size_t>(start), static_cast<size_t>(len));
                line.insert(static_cast<size_t>(start), u.newContent);
            }
        }
    }
}

// CLEANUP
void cleanExit(int code)
{
    gExit.store(true);

    bool hasRegistry = (gReg != nullptr);
    bool validSlot = (gMySlot != -1);

    if (hasRegistry && validSlot)
    {
        deregSlot(gReg, gMySlot);
    }

    clearSelfQ(gQName);

    if (hasRegistry)
    {
        ShmRegistry *tmp = gReg;
        gReg = nullptr;
        munmap(tmp, sizeof(ShmRegistry));
    }

    if (gShmFd != -1)
    {
        int fd = gShmFd;
        gShmFd = -1;
        close(fd);
    }

    exit(code);
}

void signalHandler(int signum)
{
    std::cerr << "\n[" << gUID << "] Signal " << signum << " -> cleaning up" << std::endl;

    int exitCode = 0;
    cleanExit(exitCode);
}
