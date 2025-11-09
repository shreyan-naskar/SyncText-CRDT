#include "headers.cpp"

// CONFIG
const char *SHM_NAME = "/synctext_registry_v1";
const size_t MAX_USERS = 5;
const size_t uid_LEN = 32;
const size_t NAME_QLEN = 64;
const char *BASE_DOC = "base_doc.txt";
const int POLL_INTERVAL_SEC = 2;
// Set to 1 for immediate broadcast during testing; change to 5 for spec behavior.
const int BROADCAST_BATCH_SIZE = 5;
const size_t RECV_RING_upBoundACITY = 4096;
const int MQ_MAXMSG_DEFAULT = 10;
static bool g_show_merge_message = false;
static vector<string> g_recent_notifications;

// ---------------- SHARED REGISTRY ----------------
struct UserShMem
{
    char uid[uid_LEN];
    char qName[NAME_QLEN];
    int active; // 0 or 1
};
struct ShmRegistry
{
    UserShMem users[MAX_USERS];
    int numUsers;
};

// ---------------- UPDATE (logical) ----------------
struct Update
{
    string toDo; // "insert", "delete", "replace"
    int lineNum = 0;
    int startCol = 0;
    int endCol = 0;
    string prevContent;
    string newContent;
    time_t timestamp = 0;
    string uid;
};

// ---------------- GLOBALS ----------------
ShmRegistry *gReg = nullptr;
int gShmFd = -1;
int gMySlot = -1;
string gUID;
string gQName;
mqd_t gMQ = (mqd_t)-1;
atomic<bool> gExit{false};

// Track last displayed lines for terminal stable updates and modification marking
static vector<string> gLastDispLines;
// Track latest change summaries (local diffs or merged winners) to show
static vector<Update> gPrevEdits;

// ---------------- SPSC RING (lock-free) ----------------
struct ringRecv
{
    vector<string> buffer; // store serialized messages (producer pushes strings)
    const size_t upBound;
    atomic<size_t> front, end;
    ringRecv(size_t c) : buffer(c), upBound(c), front(0), end(0) {}
    bool push(const string &s)
    {
        size_t t = end.load(memory_order_relaxed);
        size_t next = (t + 1) % upBound;
        if (next == front.load(memory_order_acquire))
            return false; // full
        buffer[t] = s;
        end.store(next, memory_order_release);
        return true;
    }
    bool pop(string &out)
    {
        size_t h = front.load(memory_order_relaxed);
        if (h == end.load(memory_order_acquire))
            return false; // empty
        out = buffer[h];
        front.store((h + 1) % upBound, memory_order_release);
        return true;
    }
};
ringRecv gRingRecv(RECV_RING_upBoundACITY);

// ---------------- UTILS ----------------
string currStr()
{
    time_t t = time(nullptr);
    char buffer[64];
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_);
    return string(buffer);
}
inline void sleepMS(int ms) { this_thread::sleep_for(chrono::milliseconds(ms)); }

// Read system mq msgsize_max (returns 0 on failure)
size_t maxSysMsgSize()
{
    FILE *f = fopen("/proc/sys/fs/mqueue/msgsize_max", "r");
    if (!f)
        return 0;
    unsigned long v = 0;
    if (fscanf(f, "%lu", &v) != 1)
    {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (size_t)v;
}

// ---------------- SHM (registry) ----------------
ShmRegistry *openReg()
{
    int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd == -1)
    {
        perror("shm_open");
        return nullptr;
    }
    size_t need = sizeof(ShmRegistry);
    if (ftruncate(fd, need) == -1)
    {
        perror("ftruncate");
        close(fd);
        return nullptr;
    }
    void *addr = mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return nullptr;
    }
    gShmFd = fd;
    return reinterpret_cast<ShmRegistry *>(addr);
}

// atomic CAS helper for shared 'active' (int)
bool atomicCASActive(int *active_ptr, int expected, int desired)
{
    return __atomic_compare_exchange_n(active_ptr, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// register user (lock-free). returns slot index or -1
int regUser(ShmRegistry *reg, const string &uid)
{
    // try reuse
    for (size_t i = 0; i < MAX_USERS; ++i)
    {
        if (strncmp(reg->users[i].uid, uid.c_str(), uid_LEN) == 0)
        {
            int prev = __atomic_load_n(&reg->users[i].active, __ATOMIC_SEQ_CST);
            if (prev == 1)
                return int(i);
            if (atomicCASActive(&reg->users[i].active, 0, 1))
                return int(i);
        }
    }
    // claim free slot
    for (size_t i = 0; i < MAX_USERS; ++i)
    {
        int prev = __atomic_load_n(&reg->users[i].active, __ATOMIC_SEQ_CST);
        if (prev == 0)
        {
            if (!atomicCASActive(&reg->users[i].active, 0, 1))
                continue;
            // fill fields
            memset(reg->users[i].uid, 0, uid_LEN);
            strncpy(reg->users[i].uid, uid.c_str(), uid_LEN - 1);
            string qn = string("/mq_") + uid;
            memset(reg->users[i].qName, 0, NAME_QLEN);
            strncpy(reg->users[i].qName, qn.c_str(), NAME_QLEN - 1);
            reg->numUsers = min<int>(MAX_USERS, reg->numUsers + 1);
            return int(i);
        }
    }
    return -1;
}
void deregSlot(ShmRegistry *reg, int slot)
{
    if (slot < 0 || slot >= int(MAX_USERS))
        return;
    __atomic_store_n(&reg->users[slot].active, 0, __ATOMIC_SEQ_CST);
    reg->users[slot].uid[0] = '\0';
    reg->users[slot].qName[0] = '\0';
    reg->numUsers = max(0, reg->numUsers - 1);
}