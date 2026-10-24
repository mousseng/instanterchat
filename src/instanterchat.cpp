// instanterchat: battle messages in the chat log as soon as their action packet arrives, instead of
// when the action's animation gets to them. The floating damage numbers keep the animation's time.
//
// How the client does it (FFXiMain RVAs):
//
// A 0x028's results go in a refcounted holder that the acting entity's scheduler keeps at +0x90:
//   holder +0x0 u16 references, +0x8 u16 results printed, +0xA u16 last printed, +0xC results list
//   list   +0x1C s16 result count, +0x20 0x34-byte results (+0x18 message), +0x24 u8 printed[64]
// Scheduler opcode 0x2B prints the next result (18C810), opcode 0x71 prints part 1 or 2 of the
// last one (18C870), and a holder prints whatever is left as its last reference goes (18C620).
// Each print is 09E090(holder, result, part), part 0 the message, 1 the additional effect, 2 the
// spikes; it spawns the floating number (056C70, unless the jne at 9E229 skips it) and then queues
// the chat line (AD660, called at 9E35D), together.
//
// What this does: holders are caught as they are attached to a scheduler (057CC0) and kept with a
// reference of our own. At the end of the frame, once the 0x028 handler has filled them in, every
// part of every result goes through 09E090 with the floating number skipped, so only the chat line
// comes out. The scheduler's own prints then run as they always did, but for results already let
// out the chat line is dropped, so only the floating number comes out, on the animation's time.
// Every change to the client is the target of a call it already makes, or one branch made a jmp for
// the length of our own calls; each is checked before it is made.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "Ashita.h"

#define CHAT_HEADER "\x1e\x51[\x1e\x06instanterchat\x1e\x51]\x1e\x01 "

#define RVA_ATTACH_SITE   0x057400 // call 057CC0, scheduler::AttachHolder(holder), in scheduler start
#define RVA_ATTACH        0x057CC0
#define RVA_FORMAT_SITE1  0x18C84C // call 09E090 in 18C810: opcode 0x2B, and the last release
#define RVA_FORMAT_SITE2  0x18C87D // call 09E090 in 18C870: opcode 0x71
#define RVA_FORMAT        0x09E090 // cdecl (holder, result, part)
#define RVA_RELEASE_SITE1 0x057537 // call 18CEA0, holder::Release(), as a scheduler is torn down
#define RVA_RELEASE_SITE2 0x0A9684
#define RVA_RELEASE       0x18CEA0
#define RVA_CHAT_SITE     0x09E35D // call AD660 in 09E090: the chat line
#define RVA_CHAT          0x0AD660
#define RVA_FLOAT_BRANCH  0x09E229 // jne 9E328 (0F 85 rel32): skips the floating number when taken
#define RVA_GAME_STATE    0x456A28 // pointer to a state whose first dword is 0x60 in game
#define HOLDERS_MAX 256
#define RESULTS_MAX 64

typedef void(__thiscall* AttachFn)(void* scheduler, void* holder);
typedef void(__thiscall* ReleaseFn)(void* holder);
typedef void(__cdecl* FormatFn)(void* holder, int result, int part);
typedef void(__cdecl* ChatFn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static AttachFn GameAttach;
static ReleaseFn GameRelease;
static FormatFn GameFormat;
static ChatFn GameChat;
static void** GameState;
static bool DropChat; // set around the scheduler's prints of results already let out

class InstanterChat;
static InstanterChat* Instance;

static void __fastcall AttachHook(void* scheduler, void* edx, void* holder);
static void __fastcall ReleaseHook(void* holder, void* edx);
static void __cdecl FormatHook(void* holder, int result, int part);
static void __cdecl ChatHook(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{
    if (!DropChat)
        GameChat(a, b, c, d, e, f, g);
}

// Points the call at site (which must be a call to expect) at to
static bool PatchCall(uintptr_t site, uintptr_t expect, uintptr_t to)
{
    uint8_t* call = (uint8_t*)site;
    if (call[0] != 0xE8 || site + 5 + *(int32_t*)(call + 1) != expect)
        return false;
    DWORD old;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(call + 1) = (int32_t)(to - (site + 5));
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

// Makes the jne that skips the floating number a jmp (nop; jmp rel32, same target), or puts it back
static void SkipFloating(uintptr_t base, bool skip)
{
    uint8_t* branch = (uint8_t*)(base + RVA_FLOAT_BRANCH);
    DWORD old;
    VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &old);
    branch[0] = skip ? 0x90 : 0x0F;
    branch[1] = skip ? 0xE9 : 0x85;
    VirtualProtect(branch, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), branch, 2);
}

struct Site {
    uint32_t site, game;
    uintptr_t hook;
};
static const Site Sites[] = {
    { RVA_ATTACH_SITE, RVA_ATTACH, (uintptr_t)AttachHook },
    { RVA_FORMAT_SITE1, RVA_FORMAT, (uintptr_t)FormatHook },
    { RVA_FORMAT_SITE2, RVA_FORMAT, (uintptr_t)FormatHook },
    { RVA_RELEASE_SITE1, RVA_RELEASE, (uintptr_t)ReleaseHook },
    { RVA_RELEASE_SITE2, RVA_RELEASE, (uintptr_t)ReleaseHook },
    { RVA_CHAT_SITE, RVA_CHAT, (uintptr_t)ChatHook },
};
#define SITES (int)(sizeof(Sites) / sizeof(Sites[0]))

class InstanterChat : public IPlugin {
    IAshitaCore* m_Core = nullptr;
    uintptr_t m_Base = 0;
    bool m_Hooked = false;  // the calls are ours, from load to unload
    bool m_Enabled = true;  // new actions are let out early; off leaves them to the scheduler

    // Holders waiting for the end of the frame, each with a reference of ours, and the results of
    // each holder already let out, whose chat lines the scheduler's prints are to drop
    void* m_Pending[HOLDERS_MAX];
    uint32_t m_PendingCount = 0;
    struct Released {
        void* holder;
        uint64_t results;
    };
    Released m_Released[HOLDERS_MAX] = {};
    uint32_t m_ReleasedNext = 0;
    uint32_t m_LetOut = 0, m_Dropped = 0;

    void Print(const char* format, ...)
    {
        char line[512];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        m_Core->GetChatManager()->Writef(0, false, "%s%s", CHAT_HEADER, line);
    }

    static uint16_t& Refs(void* holder) { return *(uint16_t*)holder; }

    Released* FindReleased(void* holder)
    {
        for (uint32_t i = 0; i < HOLDERS_MAX; ++i)
            if (m_Released[i].holder == holder)
                return &m_Released[i];
        return nullptr;
    }

    // Lets out the chat lines of each pending holder whose scheduler still has it, and lets go of
    // our reference. A result whose message is still 0 is left to the scheduler, and so is a holder
    // whose scheduler has already gone: letting go of it prints everything, as it always did.
    void ReleasePending()
    {
        if (!m_PendingCount)
            return;
        if (!*GameState || *(uint32_t*)*GameState != 0x60)
            return; // not in game: 09E090 would print nothing, so wait
        for (uint32_t h = 0; h < m_PendingCount; ++h) {
            uint8_t* holder = (uint8_t*)m_Pending[h];
            uint8_t* list = *(uint8_t**)(holder + 0xC);
            Released* known = FindReleased(holder); // let out in an earlier frame, by another attach
            const uint64_t before = known ? known->results : 0;
            uint64_t results = 0;
            if (list && Refs(holder) > 1) {
                int count = *(int16_t*)(list + 0x1C);
                if (count > RESULTS_MAX)
                    count = RESULTS_MAX;
                const uint8_t* entries = *(uint8_t**)(list + 0x20);
                SkipFloating(m_Base, true);
                for (int i = 0; i < count && entries; ++i) {
                    if (list[0x24 + i] || ((before >> i) & 1) || !*(const uint32_t*)(entries + i * 0x34 + 0x18))
                        continue; // printed already, let out already, or not filled in
                    for (int part = 0; part < 3; ++part)
                        GameFormat(holder, i, part);
                    results |= 1ull << i;
                    ++m_LetOut;
                }
                SkipFloating(m_Base, false);
            }
            if (results && known) {
                known->results |= results;
            } else if (results) {
                m_Released[m_ReleasedNext] = { holder, results };
                m_ReleasedNext = (m_ReleasedNext + 1) % HOLDERS_MAX;
            }
            OnRelease(holder);
        }
        m_PendingCount = 0;
    }

    // Points every call at our hooks, or back at the client, putting back what was done if one of
    // them is not the call it should be (another build of the client)
    bool Hook(bool on)
    {
        if (on == m_Hooked)
            return true;
        const uintptr_t base = m_Base;
        const uint8_t* branch = (const uint8_t*)(base + RVA_FLOAT_BRANCH);
        if (on && (branch[0] != 0x0F || branch[1] != 0x85))
            return false;
        if (!on) {
            for (uint32_t h = 0; h < m_PendingCount; ++h)
                OnRelease(m_Pending[h]);
            m_PendingCount = 0;
        }
        for (int i = 0; i < SITES; ++i) {
            const uintptr_t game = base + Sites[i].game;
            const bool done = on ? PatchCall(base + Sites[i].site, game, Sites[i].hook)
                                 : PatchCall(base + Sites[i].site, Sites[i].hook, game);
            if (!done) {
                for (int j = 0; j < i; ++j)
                    PatchCall(base + Sites[j].site, on ? Sites[j].hook : base + Sites[j].game,
                              on ? base + Sites[j].game : Sites[j].hook);
                return false;
            }
        }
        m_Hooked = on;
        return true;
    }

public:
    const char* GetName() const override { return "instanterchat"; }
    const char* GetAuthor() const override { return "lin"; }
    const char* GetDescription() const override { return "Battle messages in the chat log when their packet arrives, not at the animation's hit."; }
    const char* GetLink() const override { return ""; }
    double GetVersion() const override { return 1.0; }
    uint32_t GetFlags() const override
    {
        return (uint32_t)(Ashita::PluginFlags::UseCommands | Ashita::PluginFlags::UseDirect3D);
    }

    bool Initialize(IAshitaCore* core, ILogManager*, uint32_t) override
    {
        m_Core = core;
        Instance = this;
        m_Base = (uintptr_t)GetModuleHandleA("FFXiMain.dll");
        if (!m_Base)
            return false;
        GameAttach = (AttachFn)(m_Base + RVA_ATTACH);
        GameRelease = (ReleaseFn)(m_Base + RVA_RELEASE);
        GameFormat = (FormatFn)(m_Base + RVA_FORMAT);
        GameChat = (ChatFn)(m_Base + RVA_CHAT);
        GameState = (void**)(m_Base + RVA_GAME_STATE);
        if (!Hook(true))
            Print("this FFXiMain.dll is not the build instanterchat knows; nothing changed");
        return true;
    }

    void Release() override
    {
        Hook(false);
        Instance = nullptr;
    }

    // Direct3D is only for Present; the SDK's default here returns false, which fails the load
    bool Direct3DInitialize(IDirect3DDevice8*) override { return true; }

    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        if (m_Hooked)
            ReleasePending();
    }

    bool HandleCommand(int32_t, const char* command, bool) override
    {
        if (_strnicmp(command, "/instanterchat", 14) != 0 || (command[14] && command[14] != ' '))
            return false;
        const char* args = command + 14;
        while (*args == ' ')
            ++args;
        if (!_stricmp(args, "on") || !_stricmp(args, "off"))
            m_Enabled = !_stricmp(args, "on");
        if (!m_Hooked)
            Print("not active: this FFXiMain.dll is not the build instanterchat knows");
        else
            Print("%s; %u results let out early, %u later chat lines dropped. /instanterchat on | off",
                  m_Enabled ? "on" : "off", m_LetOut, m_Dropped);
        return true;
    }

    void OnAttach(void* scheduler, void* holder)
    {
        GameAttach(scheduler, holder);
        if (!m_Enabled || !holder || m_PendingCount == HOLDERS_MAX)
            return; // the last left to the scheduler
        // one action can attach its holder to several schedulers: it is let out once
        for (uint32_t h = 0; h < m_PendingCount; ++h)
            if (m_Pending[h] == holder)
                return;
        ++Refs(holder);
        m_Pending[m_PendingCount++] = holder;
    }

    // The scheduler's print of one part of one result: its chat line is dropped if already let out
    void OnFormat(void* holder, int result, int part)
    {
        const Released* r = FindReleased(holder);
        const bool drop = r && result >= 0 && result < RESULTS_MAX && ((r->results >> result) & 1);
        if (drop)
            ++m_Dropped;
        DropChat = drop;
        GameFormat(holder, result, part);
        DropChat = false;
    }

    // Forgets a holder once it is freed, and its address free to be used again: after the client's
    // release, since the last one prints what is left (18C620), and those chat lines are to be dropped
    void OnRelease(void* holder)
    {
        const bool last = Refs(holder) == 1;
        GameRelease(holder);
        if (last)
            if (Released* r = FindReleased(holder))
                r->holder = nullptr;
    }
};

static void __fastcall AttachHook(void* scheduler, void*, void* holder) { Instance->OnAttach(scheduler, holder); }
static void __fastcall ReleaseHook(void* holder, void*) { Instance->OnRelease(holder); }
static void __cdecl FormatHook(void* holder, int result, int part) { Instance->OnFormat(holder, result, part); }

extern "C" {
    IPlugin* __stdcall expCreatePlugin(const char*) { return new InstanterChat(); }
    void __stdcall expDestroyPlugin(void* instance) { delete (InstanterChat*)instance; }
    double __stdcall expGetInterfaceVersion() { return ASHITA_INTERFACE_VERSION; }
}
