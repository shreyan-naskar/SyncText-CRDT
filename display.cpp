#include "headers.cpp"
#include "file.cpp"

// DISPLAY
static string dispBoundesup(const string &s)
{
    if (s.empty())
    {
        return string("\"\"");
    }

    string out;
    out.reserve(s.size() + 2);
    out.push_back('\"');

    for (char c : s)
    {
        switch (c)
        {
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(c);
                break;
        }
    }

    out.push_back('\"');
    return out;
}

string updateClassification(const Update &u)
{
    if (u.toDo == "insert")
    {
        return "[INSERTED]";
    }

    if (u.toDo == "delete")
    {
        return "[DELETED]";
    }

    if (u.toDo == "replace")
    {
        size_t old_len = u.prevContent.size();
        size_t new_len = u.newContent.size();

        if (new_len == old_len)
        {
            return "[MODIFIED]";
        }

        return (new_len > old_len) ? "[MODIFIED]" : "[DELETED]";
    }

    return "[MODIFIED]";
}

void dispDocUpdatesSimp(const string &user_doc, const vector<string> &lines, ShmRegistry *reg)
{
    const string RESET = "\033[0m";
    const string RED   = "\033[31m";
    const string GRN   = "\033[32m";
    const string YEL   = "\033[33m";
    const string BLU   = "\033[34m";
    const string DIM   = "\033[2m";

    cout << "\033[H\033[J";
    cout << "Document: " << user_doc << "\n";
    cout << "Last updated: " << currStr() << "\n";
    cout << "----------------------------------------\n";

    size_t countPrev = gLastDispLines.size();
    size_t currCount = lines.size();
    size_t currDisp  = std::max(countPrev, currCount);

    if (gLastDispLines.size() < currDisp)
    {
        gLastDispLines.resize(currDisp);
    }

    for (size_t i = 0; i < currDisp; ++i)
    {
        const string prev = (i < gLastDispLines.size()) ? gLastDispLines[i] : string();
        const string cur  = (i < currCount) ? lines[i] : string();

        bool found_change_for_line = false;
        Update change_for_line;

        for (const auto &u : gPrevEdits)
        {
            if ((size_t)u.lineNum == i)
            {
                change_for_line = u;
                found_change_for_line = true;
                break;
            }
        }

        string outLine;

        if (found_change_for_line)
        {
            const string cls = updateClassification(change_for_line);

            if (change_for_line.toDo == "delete")
            {
                outLine = RED + string("[DELETED]") + RESET + " " + cls;
            }
            else
            {
                const string currTotLine = cur;

                int start = std::max(0, change_for_line.startCol);
                int hl_len = (int)change_for_line.newContent.size();
                if (hl_len < 0) hl_len = 0;

                if (start > (int)currTotLine.size())
                    start = (int)currTotLine.size();

                int end = start + hl_len;
                if (end > (int)currTotLine.size())
                    end = (int)currTotLine.size();

                string left  = currTotLine.substr(0, start);
                string mid   = currTotLine.substr(start, end - start);
                string right = currTotLine.substr(end);

                outLine = left + YEL + mid + RESET + right + " " + cls;
            }
        }
        else
        {
            if (cur != prev)
            {
                if (cur.empty() && !prev.empty())
                    outLine = RED + string("[DELETED]") + RESET;
                else
                    outLine = YEL + cur + RESET + " [MODIFIED]";
            }
            else
            {
                outLine = cur;
            }
        }

        cout << BLU << "Line " << i << ":" << RESET << " " << outLine << "\n";
        gLastDispLines[i] = cur;
    }

    cout << "----------------------------------------\nActive users: ";
    {
        bool first = true;
        for (size_t i = 0; i < MAX_USERS; ++i)
        {
            if (__atomic_load_n(&reg->users[i].active, __ATOMIC_SEQ_CST) == 1)
            {
                string uid = string(reg->users[i].uid);
                if (!uid.empty())
                {
                    if (!first) cout << ", ";
                    cout << uid;
                    first = false;
                }
            }
        }
    }
    cout << "\n";

    for (const auto &u : gPrevEdits)
    {
        string cls      = updateClassification(u);
        string old_disp = dispBoundesup(u.prevContent);
        string new_disp = dispBoundesup(u.newContent);

        cout << DIM << "Change detected: Line " << u.lineNum
             << ", columns " << u.startCol << "-" << std::max(u.endCol, u.startCol) << ", "
             << RESET << RED << old_disp << RESET
             << DIM << " → " << RESET
             << GRN << new_disp << RESET << " "
             << YEL << cls << RESET << "\n";
    }

    cout << "\n";

    if (g_show_merge_message && !g_recent_notifications.empty())
    {
        for (const auto &msg : g_recent_notifications)
            cout << msg << "\n";
    }
    else
    {
        cout << "Monitoring for changes...\n";
    }

    cout.flush();
}

// MESSAGE QUEUE HELPERS (robust)
size_t gMQ_msgsize = 4096; // determined at runtime
int gMQ_maxmsg = MQ_MAXMSG_DEFAULT;

bool createSelfQ(const string &qName)
{
    size_t sys_max = maxSysMsgSize();
    if (sys_max > 0)
    {
        gMQ_msgsize = std::min<size_t>(gMQ_msgsize, sys_max);
    }

    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_flags   = 0;
    attr.mq_maxmsg  = gMQ_maxmsg;
    attr.mq_msgsize = static_cast<long>(gMQ_msgsize);
    attr.mq_curmsgs = 0;

    mqd_t mq = mq_open(qName.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
    if (mq == (mqd_t)-1)
    {
        perror(("mq_open create " + qName).c_str());
        return false;
    }

    gMQ = mq;

    std::cerr << "[" << gUID << "] Created queue: " << qName
              << " (msgsize=" << attr.mq_msgsize << ")\n";

    return true;
}

bool sendRetriesUpdatesToQ(const string &qName, const string &msg, int retries, int delay_ms)
{
    for (int attempt = 0; attempt < retries && !gExit.load(); ++attempt)
    {
        mqd_t mq = mq_open(qName.c_str(), O_WRONLY);
        if (mq == (mqd_t)-1)
        {
            // Queue does not exist or is not accessible yet; retry after delay
            if (errno == ENOENT || errno == EACCES)
            {
                sleepMS(delay_ms);
                continue;
            }

            // Any other error: also pause and retry
            sleepMS(delay_ms);
            continue;
        }

        if (msg.size() > gMQ_msgsize)
        {
            cerr << "[" << gUID << "] ERROR: message too large for mq (size="
                 << msg.size() << " max=" << gMQ_msgsize << ")\n";
            mq_close(mq);
            return false;
        }

        int ret = mq_send(mq, msg.data(), msg.size(), 0);
        if (ret == -1)
        {
            perror(("mq_send " + qName).c_str());
            mq_close(mq);
            sleepMS(delay_ms);
            continue;
        }

        mq_close(mq);
        cerr << "[" << gUID << "] Sent to " << qName << " ("
             << msg.size() << " bytes)\n";
        return true;
    }

    cerr << "[" << gUID << "] WARN: failed to send to "
         << qName << " after retries\n";
    return false;
}
void clearSelfQ(const string &qName)
{
    if (gMQ != (mqd_t)-1)
    {
        mqd_t tmp = gMQ;
        gMQ = (mqd_t)-1;
        mq_close(tmp);
    }

    mq_unlink(qName.c_str());
}
