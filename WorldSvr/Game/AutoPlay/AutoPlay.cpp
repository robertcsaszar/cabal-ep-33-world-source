#include "AutoPlay.h"
#include "Common/LibUtils.h"
#include "Game/Management/Management.h"
#include <cmath>
#include <ctime>
#include <cstring>
#include <cctype>

extern AutoPlay* g_pAutoPlay = AutoPlay::GetInstance();

// Faza 1b: atac real. Layout-ul C2S_ATTCKTOMOBS si adresa handler-ului au fost
// extrase din binarul WorldSvr EP33 (OnCSCAttckToMobs @ 0x007456C0):
//   - pachet = 16 octeti
//   - offset 0x0a: DWORD index tinta
//   - offset 0x0e: BYTE tip obiect
//   - offset 0x0f: BYTE flag world-mob
//
// Ramane 0 pana confirmam complet Faza 1a.
#define AUTOPLAY_ENABLE_ACTIONS 0

namespace
{
    // Raza de scanare in unitatile serverului.
    constexpr int kScanRadius = 200;

    // Prag HP pentru diagnostic.
    constexpr int kLowHpPercent = 30;

    const std::string kLogPath = "./AutoPlay.log";
}

void AutoPlay::Init()
{
    Management::WriteLogs(
        kLogPath,
        "AutoPlay::Init(): register CSC_HEARTBEAT + CSC_GMCOMMAND"
    );

    REGISTER_PROC(
        g_sUsrProcedureMap,
        MAINCMD_VALUE_EX::CSC_HEARTBEAT,
        OnHeartbeat
    );

    REGISTER_PROC(
        g_sUsrProcedureMap,
        MAINCMD_VALUE_EX::CSC_GMCOMMAND,
        OnGMCommand
    );
}

bool AutoPlay::PayloadContains(
    const char* buf,
    int len,
    const char* token
)
{
    if (!buf || len <= 0 || !token)
        return false;

    const int tlen = static_cast<int>(strlen(token));

    if (tlen == 0 || tlen > len)
        return false;

    for (int i = 0; i + tlen <= len; ++i)
    {
        int k = 0;

        for (; k < tlen; ++k)
        {
            const char a = static_cast<char>(
                tolower(static_cast<unsigned char>(buf[i + k]))
            );

            const char b = static_cast<char>(
                tolower(static_cast<unsigned char>(token[k]))
            );

            if (a != b)
                break;
        }

        if (k == tlen)
            return true;
    }

    return false;
}

void AutoPlay::Set(DWORD userNum, bool on)
{
    m_states[userNum].enabled = on;
}

bool AutoPlay::IsEnabled(DWORD userNum)
{
    auto it = m_states.find(userNum);

    return
        it != m_states.end() &&
        it->second.enabled;
}

bool AutoPlay::Toggle(DWORD userNum)
{
    const bool now = !IsEnabled(userNum);

    m_states[userNum].enabled = now;

    return now;
}

int AutoPlay::FindNearestMob(
    USERDATACONTEXT* pUserDataCtx,
    int radius,
    long long* outDistSq
)
{
    CWorld* pWorld = pUserDataCtx->sPosData.pWorld;

    if (!pWorld)
        return -1;

    const int px = pUserDataCtx->sPosData.iPosXCur;
    const int py = pUserDataCtx->sPosData.iPosYCur;

    const long long radSq =
        static_cast<long long>(radius) *
        static_cast<long long>(radius);

    int bestRow = -1;
    long long bestSq = radSq + 1;

    for (int row = 0; row < pWorld->iMobsCount; ++row)
    {
        MOBSCONTEXT* pMob = pWorld->GetMobPtr(row);

        if (!pMob)
            continue;

        if (pMob->bIsDead)
            continue;

        if (pMob->sParameters.iHP <= 0)
            continue;

        const long long dx =
            static_cast<long long>(
                pMob->sPosData.iPosXCur
            ) - px;

        const long long dy =
            static_cast<long long>(
                pMob->sPosData.iPosYCur
            ) - py;

        const long long d2 =
            dx * dx +
            dy * dy;

        if (d2 < bestSq)
        {
            bestSq = d2;
            bestRow = row;
        }
    }

    if (outDistSq)
    {
        *outDistSq =
            bestRow >= 0
                ? bestSq
                : -1;
    }

    return bestRow;
}

#if AUTOPLAY_ENABLE_ACTIONS

#pragma pack(push, 1)

struct C2S_ATTCKTOMOBS_PKT
{
    WORD  wMagicCode;
    WORD  wPayLoadLen;
    DWORD dwCheckSum;
    WORD  wMainCmd;

    WORD  wTargetIdx;
    WORD  wTargetIdxHi;
    BYTE  bTargetType;
    BYTE  bWorldMob;
};

#pragma pack(pop)

static_assert(
    sizeof(C2S_ATTCKTOMOBS_PKT) == 0x10,
    "attack packet must be 16 bytes"
);

typedef int (*OnCSCAttckToMobs_t)(
    long long pProcessLayer,
    PROCESSDATACONTEXT* pCtx
);

static OnCSCAttckToMobs_t OnCSCAttckToMobs =
    reinterpret_cast<OnCSCAttckToMobs_t>(
        0x007456C0
    );

static void TryAttack(
    USERCONTEXT* pUserCtx,
    MOBSCONTEXT* pMob
)
{
    C2S_ATTCKTOMOBS_PKT pkt = {};

    pkt.wMagicCode =
        MAGIC_CODE;

    pkt.wPayLoadLen =
        sizeof(pkt);

    pkt.wMainCmd =
        MAINCMD_VALUE_EX::CSC_ATTCKTOMOBS;

    pkt.wTargetIdx =
        static_cast<WORD>(
            pMob->objIdx.sObjIdxData
        );

    pkt.wTargetIdxHi = 0;

    pkt.bTargetType =
        pMob->objIdx.objectType;

    pkt.bWorldMob =
        pMob->objIdx.bWorldMob;

    PROCESSDATACONTEXT ctx = {};

    ctx.pUserCtx =
        reinterpret_cast<int*>(
            pUserCtx
        );

    ctx.cpPacket =
        reinterpret_cast<char*>(
            &pkt
        );

    ctx.iLen =
        sizeof(pkt);

    OnCSCAttckToMobs(
        0,
        &ctx
    );
}

#endif

void AutoPlay::Tick(
    USERCONTEXT* pUserCtx,
    USERDATACONTEXT* pUserDataCtx
)
{
    State& st =
        m_states[
            pUserDataCtx->GetUserNum()
        ];

    long long distSq = -1;

    const int row =
        FindNearestMob(
            pUserDataCtx,
            kScanRadius,
            &distSq
        );

    st.targetMobRow = row;

    const long long hp =
        pUserDataCtx->sParameters.iHP;

    const long long hpMax =
        pUserDataCtx->sParameters.iHPMax;

    const int hpPct =
        hpMax > 0
            ? static_cast<int>(
                (hp * 100) / hpMax
              )
            : 0;

    const long nowSec =
        static_cast<long>(
            time(nullptr)
        );

    if (nowSec != st.lastLogSec)
    {
        st.lastLogSec = nowSec;

        const long long dist =
            distSq >= 0
                ? static_cast<long long>(
                    std::sqrt(
                        static_cast<double>(
                            distSq
                        )
                    )
                  )
                : -1;

        char line[256];

        snprintf(
            line,
            sizeof(line),
            "user=%u char=%u pos=(%d,%d) hp=%d%% nearestMobRow=%d dist=%lld lowHp=%d",
            pUserDataCtx->GetUserNum(),
            pUserDataCtx->GetCharacterIdx(),
            pUserDataCtx->sPosData.iPosXCur,
            pUserDataCtx->sPosData.iPosYCur,
            hpPct,
            row,
            dist,
            hpPct <= kLowHpPercent ? 1 : 0
        );

        Management::WriteLogs(
            kLogPath,
            line
        );
    }

#if AUTOPLAY_ENABLE_ACTIONS

    if (row >= 0)
    {
        MOBSCONTEXT* pMob =
            pUserDataCtx
                ->sPosData
                .pWorld
                ->GetMobPtr(row);

        if (pMob)
        {
            TryAttack(
                pUserCtx,
                pMob
            );
        }
    }

#endif
}

int AutoPlay::OnHeartbeat(
    int* pProcessLayer,
    PROCESSDATACONTEXT* pProcessDataCtx
)
{
    ALIAS_PTR(
        USERCONTEXT,
        pUserCtx,
        pProcessDataCtx->pUserCtx
    );

    ALIAS_PTR(
        USERDATACONTEXT,
        pUserDataCtx,
        pUserCtx->pData
    );

    if (!pUserDataCtx->bIsActvteLink)
        return P_OK;

    {
        static long lastDiag = 0;

        const long now =
            static_cast<long>(
                time(nullptr)
            );

        if (now - lastDiag >= 5)
        {
            lastDiag = now;

            char line[160];

            snprintf(
                line,
                sizeof(line),
                "DIAG heartbeat OK user=%u char=%u autoplay=%d",
                pUserDataCtx->GetUserNum(),
                pUserDataCtx->GetCharacterIdx(),
                g_pAutoPlay->IsEnabled(
                    pUserDataCtx->GetUserNum()
                ) ? 1 : 0
            );

            Management::WriteLogs(
                kLogPath,
                line
            );
        }
    }

    if (
        g_pAutoPlay->IsEnabled(
            pUserDataCtx->GetUserNum()
        )
    )
    {
        g_pAutoPlay->Tick(
            pUserCtx,
            pUserDataCtx
        );
    }

    return P_OK;
}

int AutoPlay::OnGMCommand(
    int* pProcessLayer,
    PROCESSDATACONTEXT* pProcessDataCtx
)
{
    ALIAS_PTR(
        USERCONTEXT,
        pUserCtx,
        pProcessDataCtx->pUserCtx
    );

    ALIAS_PTR(
        USERDATACONTEXT,
        pUserDataCtx,
        pUserCtx->pData
    );

    const unsigned char* packet =
        reinterpret_cast<const unsigned char*>(
            pProcessDataCtx->cpPacket
        );

    const int len =
        static_cast<int>(
            pProcessDataCtx->iLen
        );

    if (!packet || len <= 0)
        return P_OK;

    // HEX dump complet al pachetului.
    {
        char hex[512];
        int pos = 0;

        for (
            int i = 0;
            i < len &&
            pos < static_cast<int>(sizeof(hex)) - 4;
            ++i
        )
        {
            const int written =
                snprintf(
                    hex + pos,
                    sizeof(hex) - pos,
                    "%02X ",
                    packet[i]
                );

            if (written <= 0)
                break;

            pos += written;
        }

        if (
            pos >=
            static_cast<int>(sizeof(hex))
        )
        {
            pos =
                static_cast<int>(
                    sizeof(hex)
                ) - 1;
        }

        hex[pos] = '\0';

        char line[640];

        snprintf(
            line,
            sizeof(line),
            "DIAG GMCMD user=%u char=%u len=%d hex=%s",
            pUserDataCtx->GetUserNum(),
            pUserDataCtx->GetCharacterIdx(),
            len,
            hex
        );

        Management::WriteLogs(
            kLogPath,
            line
        );
    }

    if (!pUserDataCtx->bIsActvteLink)
        return P_OK;

    /*
        IMPORTANT:

        Nu mai schimbam momentan starea AutoPlay aici.

        Am observat ca o singura comanda:

            /autoplay on

        produce cel putin doua pachete:

            ... EA 01 01 01
            ... EA 01 01 00

        Prin urmare byte-ul final NU poate fi tratat inca drept
        "1 = autoplay on" si "0 = autoplay off".

        Lasam acest handler doar in mod diagnostic pana aflam
        layout-ul real al CSC_GMCOMMAND.
    */

    if (len >= 12)
    {
        const BYTE commandByte =
            packet[10];

        const BYTE stateByte =
            packet[11];

        char line[192];

        snprintf(
            line,
            sizeof(line),
            "DIAG GMCMD parsed user=%u char=%u byte10=0x%02X byte11=0x%02X",
            pUserDataCtx->GetUserNum(),
            pUserDataCtx->GetCharacterIdx(),
            commandByte,
            stateByte
        );

        Management::WriteLogs(
            kLogPath,
            line
        );
    }

    return P_OK;
}