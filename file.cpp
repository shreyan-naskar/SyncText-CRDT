#include "headers.cpp"
#include "globals.cpp"

// FILE helpers
vector<string> readLinesFile(const string &filename)
{
    vector<string> lines;
    ifstream ifs(filename);
    if (!ifs.is_open())
        return lines;
    string line;
    while (getline(ifs, line))
        lines.push_back(line);
    return lines;
}
void writeLinesFile(const string &filename, const vector<string> &lines)
{
    ofstream ofs(filename, ios::trunc);
    for (const auto &L : lines)
        ofs << L << "\n";
}
void verifyLocalDoc(const string &user_doc)
{
    struct stat st;
    if (stat(user_doc.c_str(), &st) == 0)
        return;
    ifstream src(BASE_DOC);
    ofstream dst(user_doc);
    if (!src.is_open())
    {
        // default content
        dst << "Hello User!\n";
        dst << "Start making changes.\n";
        dst << "See real-time updates!\n";
        dst << "Come collaborate with others.\n";
        return;
    }
    string line;
    while (getline(src, line))
        dst << line << "\n";
}

// DIFF: produce Update objects
vector<Update> diffLinesMakeUpdates(const vector<string> &old_lines, const vector<string> &new_lines, const string &uid)
{
    vector<Update> updates;
    size_t old_n = old_lines.size();
    size_t new_n = new_lines.size();
    size_t max_n = max(old_n, new_n);

    for (size_t i = 0; i < max_n; ++i)
    {
        string oldL = (i < old_n) ? old_lines[i] : string();
        string newL = (i < new_n) ? new_lines[i] : string();
        if (oldL == newL)
            continue;

        Update u;
        u.lineNum = (int)i;
        u.timestamp = time(nullptr);
        u.uid = uid;

        if (oldL.empty() && !newL.empty())
        {
            u.toDo = "insert";
            u.startCol = 0;
            u.endCol = 0;
            u.newContent = newL;
            updates.push_back(u);
            continue;
        }
        if (!oldL.empty() && newL.empty())
        {
            u.toDo = "delete";
            u.startCol = 0;
            u.endCol = (int)oldL.size();
            u.prevContent = oldL;
            updates.push_back(u);
            continue;
        }

        // ----- IMPROVED REPLACE DIFF -----
        int oN = oldL.size(), nN = newL.size();
        int prefix = 0;

        // Find longest common prefix
        while (prefix < oN && prefix < nN && oldL[prefix] == newL[prefix])
            prefix++;

        // Find longest common suffix
        int suffix = 0;
        while (suffix < (oN - prefix) && suffix < (nN - prefix) &&
               oldL[oN - suffix - 1] == newL[nN - suffix - 1])
            suffix++;

        int start = prefix;
        int end_old = oN - suffix;
        int end_new = nN - suffix;

        string old_mid = oldL.substr(start, end_old - start);
        string new_mid = newL.substr(start, end_new - start);

        // ---------- KEY FIX: If old_mid is empty, expand left until previous space ----------
        if (old_mid.empty() && start > 0)
        {
            int expand = start - 1;
            while (expand > 0 && oldL[expand - 1] != ' ')
                expand--;

            // Now expand replacement range leftward
            old_mid = oldL.substr(expand, end_old - expand);
            new_mid = newL.substr(expand, end_new - expand);
            start = expand;
        }

        // Construct update
        u.toDo = "replace";
        u.startCol = start;
        u.endCol = start + old_mid.size();
        u.prevContent = old_mid;
        u.newContent = new_mid;
        updates.push_back(u);
        continue;
    }

    return updates;
}

// SERIALIZATION (compact)
// Format:
// toDo|line|startCol|endCol|timestamp|uid|old_len|old|new_len|new
// using '|' as separators and lengths to allow any char in old/new.
string serialize_update(const Update &u)
{
    string s;
    s.reserve(256 + u.prevContent.size() + u.newContent.size());
    s += u.toDo;
    s += '|';
    s += to_string(u.lineNum);
    s += '|';
    s += to_string(u.startCol);
    s += '|';
    s += to_string(u.endCol);
    s += '|';
    s += to_string((long long)u.timestamp);
    s += '|';
    s += u.uid;
    s += '|';
    s += to_string(u.prevContent.size());
    s += '|';
    s += u.prevContent;
    s += '|';
    s += to_string(u.newContent.size());
    s += '|';
    s += u.newContent;
    return s;
}
bool updateDeserialize(const string &s, Update &out)
{
    size_t pos = 0, next = 0;
    auto extractToken = [&](string &tok) -> bool
    {
        next = s.find('|', pos);
        if (next == string::npos)
            return false;
        tok = s.substr(pos, next - pos);
        pos = next + 1;
        return true;
    };
    string tok;
    if (!extractToken(tok))
        return false;
    out.toDo = tok;
    if (!extractToken(tok))
        return false;
    out.lineNum = stoi(tok);
    if (!extractToken(tok))
        return false;
    out.startCol = stoi(tok);
    if (!extractToken(tok))
        return false;
    out.endCol = stoi(tok);
    if (!extractToken(tok))
        return false;
    out.timestamp = (time_t)stoll(tok);
    if (!extractToken(tok))
        return false;
    out.uid = tok;

    if (!extractToken(tok))
        return false; // old_len
    size_t old_len = (size_t)stoul(tok);
    if (pos + old_len > s.size())
        return false;
    out.prevContent = s.substr(pos, old_len);
    pos += old_len;

    if (pos >= s.size() || s[pos] != '|')
        return false;
    pos++;

    next = s.find('|', pos);
    if (next == string::npos)
        return false;
    string new_len_tok = s.substr(pos, next - pos);
    pos = next + 1;
    size_t new_len = (size_t)stoul(new_len_tok);
    if (pos + new_len > s.size())
        return false;
    out.newContent = s.substr(pos, new_len);
    return true;
}
