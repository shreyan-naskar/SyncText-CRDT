#include "headers.cpp"
#include "display.cpp"


// ---------------- LISTENER THREAD ----------------
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
    cerr << "[" << gUID << "] Listener running on " << qName << "\n";
    vector<char> buffer(gMQ_msgsize + 10);
    while (!gExit.load())
    {
        ssize_t n = mq_receive(mq, buffer.data(), buffer.size(), nullptr);
        if (n >= 0)
        {
            string s(buffer.data(), (size_t)n);
            bool pushed = gRingRecv.push(s);
            if (!pushed)
            {
                cerr << "[" << gUID << "] WARN: recv ring full, dropping message\n";
            }
            else
            {
                Update tmp;
                if (updateDeserialize(s, tmp))
                {
                    // Record receive notification; don't touch gPrevEdits here
                    g_recent_notifications.push_back(
                        "Received update from " + tmp.uid + ": Line " + to_string(tmp.lineNum) + " modified");
                    g_show_merge_message = true;
                }
                else
                {
                    cerr << "[" << gUID << "] Received (badly formed) message\n";
                }
            }
        }
        else
        {
            if (errno == EINTR)
                continue;
            sleepMS(50);
        }
    }
    mq_close(mq);
}

// ---------------- CRDT MERGE (LWW) ----------------
bool collisionUpdates(const Update &a, const Update &b)
{
    if (a.lineNum != b.lineNum)
        return false;

    int aStart = min(a.startCol, a.endCol);
    int aEnd = max(a.startCol, a.endCol);
    int bStart = min(b.startCol, b.endCol);
    int bEnd = max(b.startCol, b.endCol);

    int aLen = max(0, aEnd - aStart);
    int bLen = max(0, bEnd - bStart);

    if (aLen == 0 && bLen == 0)
        return aStart == bStart; // same insertion point
    if (aLen == 0 && bLen > 0)
        return (aStart >= bStart && aStart < bEnd);
    if (bLen == 0 && aLen > 0)
        return (bStart >= aStart && bStart < aEnd);
    return (aStart < bEnd && bStart < aEnd); // half-open overlap
}
bool updatesAonB(const Update &a, const Update &b)
{
    if (a.timestamp != b.timestamp)
        return a.timestamp > b.timestamp; // latest wins
    return a.uid < b.uid;         // tie-break by uid (deterministic)
}

vector<Update> crdtMerge(const vector<Update> &all)
{
    size_t n = all.size();
    vector<bool> keep(n, true);
    for (size_t i = 0; i < n; ++i)
    {
        if (!keep[i])
            continue;
        for (size_t j = i + 1; j < n; ++j)
        {
            if (!keep[j])
                continue;
            if (collisionUpdates(all[i], all[j]))
            {
                if (updatesAonB(all[i], all[j]))
                    keep[j] = false;
                else
                    keep[i] = false;
            }
        }
    }
    vector<Update> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            out.push_back(all[i]);
    return out;
}

void applyLineUpdates(vector<string> &lines, const vector<Update> &wins)
{
    for (const auto &u : wins)
    {
        if (u.lineNum < 0)
            continue;
        if ((size_t)u.lineNum >= lines.size())
            lines.resize(u.lineNum + 1);
        if (u.toDo == "delete")
        {
            int start = max(0, u.startCol);
            int len = max(0, u.endCol - u.startCol);
            if (start < (int)lines[u.lineNum].size())
                lines[u.lineNum].erase(start, len);
        }
        else if (u.toDo == "insert")
        {
            int pos = max(0, u.startCol);
            if (pos > (int)lines[u.lineNum].size())
                pos = lines[u.lineNum].size();
            lines[u.lineNum].insert(pos, u.newContent);
        }
        else if (u.toDo == "replace")
        {
            int start = max(0, u.startCol);
            int len = max(0, u.endCol - u.startCol);
            if (start > (int)lines[u.lineNum].size())
                start = lines[u.lineNum].size();
            lines[u.lineNum].erase(start, len);
            lines[u.lineNum].insert(start, u.newContent);
        }
    }
}

// ---------------- CLEANUP ----------------
void cleanExit(int code)
{
    gExit.store(true);
    if (gReg && gMySlot != -1)
        deregSlot(gReg, gMySlot);
    clearSelfQ(gQName);
    if (gReg)
    {
        munmap(gReg, sizeof(ShmRegistry));
        gReg = nullptr;
    }
    if (gShmFd != -1)
    {
        close(gShmFd);
        gShmFd = -1;
    }
    exit(code);
}
void signalHandler(int signum)
{
    cerr << "\n[" << gUID << "] Signal " << signum << " -> cleaning up\n";
    cleanExit(0);
}
