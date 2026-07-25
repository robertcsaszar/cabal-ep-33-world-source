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

    // Probe non-destructiv pentru canalele de mesaj cunoscute din Protodefs.h.
    // Handlerul nostru returneaza mereu P_OK, deci procedurile native raman active.
    // Trigger server-side fara modificare de client, pe NORMAL CHAT.
    // REQ_MESSAGEEVNT = 195 este pachetul client -> WorldSvr pentru mesajul normal,
    // iar NFY_MESSAGEEVNT = 217 este notificarea asociata. Folosim !autoplay
    // pentru a evita parserul client-side al comenzilor care incep cu '/'.
    REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::REQ_MESSAGEEVNT, OnMessageProbe);

    // Pastram si probele cunoscute pentru PM / loud message; sunt non-destructive.
    REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::REQ_LOUDMSGCHANNEL, OnMessageProbe);
    REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::REQ_LOUDMSGSERVER, OnMessageProbe);
    REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::REQ_LOUDMSGSERVER2, OnMessageProbe);
    REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::C2S_SENDPMMESSAGE, OnMessageProbe);

    Management::WriteLogs(
        kLogPath,
        "AutoPlay::Init(): normal chat trigger 195 + message probes 393/395/396/483 registered"
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

bool AutoPlay::PayloadContainsUtf16LE(
    const char* buf,
    int len,
    const char* token
)
{
    if (!buf || len <= 0 || !token)
        return false;

    const int tlen = static_cast<int>(strlen(token));
    if (tlen == 0 || (tlen * 2) > len)
        return false;

    for (int i = 0; i + (tlen * 2) <= len; ++i)
    {
        int k = 0;
        for (; k < tlen; ++k)
        {
            const unsigned char lo = static_cast<unsigned char>(buf[i + k * 2]);
            const unsigned char hi = static_cast<unsigned char>(buf[i + k * 2 + 1]);
            const unsigned char want = static_cast<unsigned char>(token[k]);

            if (hi != 0 || tolower(lo) != tolower(want))
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

    if (g_pAutoPlay->IsEnabled(pUserDataCtx->GetUserNum()))
	{
		// =========================================================
		// Folosim offseturile confirmate din binarul WorldSvr EP33
		// =========================================================

		char* playerBase =
			reinterpret_cast<char*>(pUserDataCtx);

		// Pozitia reala a playerului:
		// +0x120 = PosX
		// +0x124 = PosY
		const int posX =
			*reinterpret_cast<int*>(
				playerBase + 0x120
			);

		const int posY =
			*reinterpret_cast<int*>(
				playerBase + 0x124
			);

		// World* real:
		// +0x190 = pWorld
		CWorld* pWorld =
			*reinterpret_cast<CWorld**>(
				playerBase + 0x190
			);

		// HP ramane momentan din structura actuala,
		// fiindca am confirmat deja ca valorile sunt corecte.
		const long long hp =
			pUserDataCtx->sParameters.iHP;

		const long long hpMax =
			pUserDataCtx->sParameters.iHPMax;

		char diag1[256];

		snprintf(
			diag1,
			sizeof(diag1),
			"DIAG real world test: user=%u char=%u hp=%lld/%lld pos=(%d,%d) world=%p",
			pUserDataCtx->GetUserNum(),
			pUserDataCtx->GetCharacterIdx(),
			hp,
			hpMax,
			posX,
			posY,
			static_cast<void*>(pWorld)
		);

		Management::WriteLogs(
			kLogPath,
			diag1
		);

		if (!pWorld)
		{
			Management::WriteLogs(
				kLogPath,
				"DIAG real world test: pWorld NULL"
			);

			return P_OK;
		}

		// =========================================================
		// Layout CWorld corect pentru binarul EP33 64-bit.
		// Sursa: GameStructs.h (struct CWorld) coroborat cu reperul
		// iWorldIdx@0x194 care se potriveste si cu RE-ul:
		//
		//   +0x194 = iWorldIdx
		//   +0x19C = iMobsCount
		//   +0x1B0 = m_pMobsCtx (pointer catre array-ul de mobs)
		//
		// Offset-urile vechi +0xC0/+0xD8 erau GRESITE: la runtime dadeau
		// nil / pointer-catre-world, nu array+count.
		// =========================================================

		char* worldBase =
			reinterpret_cast<char*>(pWorld);

		const int worldIdx =
			*reinterpret_cast<int*>(worldBase + 0x194);

		const int mobsCount =
			*reinterpret_cast<int*>(worldBase + 0x19C);

		void* mobArray =
			*reinterpret_cast<void**>(worldBase + 0x1B0);

		char diag2[256];

		snprintf(
			diag2,
			sizeof(diag2),
			"DIAG real world test: worldIdx=%d mobsCount=%d m_pMobsCtx=%p",
			worldIdx,
			mobsCount,
			mobArray
		);

		Management::WriteLogs(
			kLogPath,
			diag2
		);

		// Dump brut world+0x180..0x1C0 (qword) ca sa confirmam vizual
		// unde stau count-ul si pointer-ul de mobs pe binarul asta.
		{
			char dump[512];
			int p = 0;

			for (int off = 0x180; off <= 0x1C0; off += 8)
			{
				const unsigned long long q =
					*reinterpret_cast<unsigned long long*>(
						worldBase + off
					);

				const int w = snprintf(
					dump + p,
					sizeof(dump) - p,
					"+0x%X=%016llX ",
					off,
					q
				);

				if (w <= 0)
					break;

				p += w;

				if (p >= static_cast<int>(sizeof(dump)) - 24)
					break;
			}

			dump[p] = 0;

			char line[600];

			snprintf(
				line,
				sizeof(line),
				"DIAG world dump: %s",
				dump
			);

			Management::WriteLogs(kLogPath, line);
		}

		// =========================================================
		// Enumerare DIRECTA a mobs prin m_pMobsCtx (world+0x1B0),
		// stride = 0xDF90 (dimensiunea unui actor, confirmata din RE).
		// NU folosim accesorul nativ 0x00800B50: la runtime a intors nil
		// fiindca intern citeste offset-urile gresite 0xC0/0xD8.
		//
		// Offset-uri camp mob (din RE, MOBSCONTEXT/CREATUREBASE):
		//   +0x120/+0x124 pos, +0x130/+0x134 cell,
		//   +0x5F8/+0x5FC HP cur/max, +0x850 species, +0x854 objIdx,
		//   +0x1568 dead flag, +0x1570 in-world/active flag.
		// =========================================================

		char* mobBase =
			reinterpret_cast<char*>(mobArray);

		const long long kMobStride = 0xDF90;

		if (mobBase && mobsCount > 0 && mobsCount <= 4096)
		{
			// (1) Dump primele 3 sloturi ca sa validam stride + offset-uri.
			for (int i = 0; i < 3 && i < mobsCount; ++i)
			{
				char* p =
					mobBase + static_cast<long long>(i) * kMobStride;

				const int species = *reinterpret_cast<int*>(p + 0x850);
				const int objIdx  = *reinterpret_cast<int*>(p + 0x854);
				const int mhp     = *reinterpret_cast<int*>(p + 0x5F8);
				const int mhpMax  = *reinterpret_cast<int*>(p + 0x5FC);
				const unsigned char dead =
					*reinterpret_cast<unsigned char*>(p + 0x1568);
				const unsigned char active =
					*reinterpret_cast<unsigned char*>(p + 0x1570);
				const int mpx = *reinterpret_cast<int*>(p + 0x120);
				const int mpy = *reinterpret_cast<int*>(p + 0x124);
				const int mcx = *reinterpret_cast<int*>(p + 0x130);
				const int mcy = *reinterpret_cast<int*>(p + 0x134);

				char line[320];
				snprintf(
					line,
					sizeof(line),
					"DIAG mob[%d] @%p species=%d objIdx=%d hp=%d/%d "
					"dead=%u active=%u pos=(%d,%d) cell=(%d,%d)",
					i,
					static_cast<void*>(p),
					species, objIdx, mhp, mhpMax,
					dead, active, mpx, mpy, mcx, mcy
				);
				Management::WriteLogs(kLogPath, line);
			}

			// (2) Scan complet: numara mobii vii si gaseste cel mai apropiat
			//     fata de pozitia playerului (posX/posY citite mai sus).
			int       aliveCount  = 0;
			int       nearestObj  = -1;
			int       nearestRow  = -1;
			long long nearestD2   = -1;

			for (int i = 0; i < mobsCount; ++i)
			{
				char* p =
					mobBase + static_cast<long long>(i) * kMobStride;

				const unsigned char dead =
					*reinterpret_cast<unsigned char*>(p + 0x1568);
				const unsigned char active =
					*reinterpret_cast<unsigned char*>(p + 0x1570);
				const int mhp = *reinterpret_cast<int*>(p + 0x5F8);

				if (dead || !active || mhp <= 0)
					continue;

				++aliveCount;

				const long long dx =
					static_cast<long long>(
						*reinterpret_cast<int*>(p + 0x120)
					) - posX;
				const long long dy =
					static_cast<long long>(
						*reinterpret_cast<int*>(p + 0x124)
					) - posY;
				const long long d2 = dx * dx + dy * dy;

				if (nearestD2 < 0 || d2 < nearestD2)
				{
					nearestD2  = d2;
					nearestRow = i;
					nearestObj = *reinterpret_cast<int*>(p + 0x854);
				}
			}

			char sum[256];
			snprintf(
				sum,
				sizeof(sum),
				"DIAG scan: alive=%d/%d nearestRow=%d nearestObj=%d dist=%lld",
				aliveCount,
				mobsCount,
				nearestRow,
				nearestObj,
				nearestD2 >= 0
					? static_cast<long long>(std::sqrt(
						static_cast<double>(nearestD2)))
					: -1
			);
			Management::WriteLogs(kLogPath, sum);
		}
		else
		{
			Management::WriteLogs(
				kLogPath,
				"DIAG scan: m_pMobsCtx invalid, skip"
			);
		}

		// STOP aici momentan. Doar diagnostic; fara atac/miscare.
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

int AutoPlay::OnMessageProbe(
    int* pProcessLayer,
    PROCESSDATACONTEXT* pProcessDataCtx
)
{
    ALIAS_PTR(USERCONTEXT, pUserCtx, pProcessDataCtx->pUserCtx);
    ALIAS_PTR(USERDATACONTEXT, pUserDataCtx, pUserCtx->pData);

    const char* raw = pProcessDataCtx->cpPacket;
    const int len = static_cast<int>(pProcessDataCtx->iLen);

    if (!raw || len <= 0)
        return P_OK;

    const unsigned char* packet =
        reinterpret_cast<const unsigned char*>(raw);

    WORD mainCmd = 0;
    if (len >= static_cast<int>(sizeof(C2S_HEADER)))
    {
        const C2S_HEADER* hdr =
            reinterpret_cast<const C2S_HEADER*>(raw);
        mainCmd = hdr->wMainCmd;
    }

    char hex[768];
    char ascii[384];
    int hpos = 0;
    int apos = 0;

    for (int i = 0; i < len; ++i)
    {
        if (hpos < static_cast<int>(sizeof(hex)) - 4)
        {
            const int written = snprintf(
                hex + hpos,
                sizeof(hex) - hpos,
                "%02X ",
                packet[i]
            );
            if (written > 0)
                hpos += written;
        }

        if (apos < static_cast<int>(sizeof(ascii)) - 1)
        {
            const unsigned char c = packet[i];
            ascii[apos++] =
                (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
    }

    hex[hpos] = 0;
    ascii[apos] = 0;

    char line[1400];
    snprintf(
        line,
        sizeof(line),
        "DIAG MSG maincmd=%u user=%u char=%u len=%d ascii='%s' hex=%s",
        static_cast<unsigned int>(mainCmd),
        pUserDataCtx->GetUserNum(),
        pUserDataCtx->GetCharacterIdx(),
        len,
        ascii,
        hex
    );
    Management::WriteLogs(kLogPath, line);

    if (!pUserDataCtx->bIsActvteLink)
        return P_OK;

    const bool hasAutoPlay =
        PayloadContains(raw, len, "!autoplay") ||
        PayloadContainsUtf16LE(raw, len, "!autoplay");

    if (!hasAutoPlay)
        return P_OK;

    const bool hasOff =
        PayloadContains(raw, len, "off") ||
        PayloadContainsUtf16LE(raw, len, "off");

    const bool hasOn =
        PayloadContains(raw, len, "on") ||
        PayloadContainsUtf16LE(raw, len, "on");

    if (!hasOn && !hasOff)
        return P_OK;

    const bool on = hasOn && !hasOff;
    g_pAutoPlay->Set(pUserDataCtx->GetUserNum(), on);

    char stateLine[192];
    snprintf(
        stateLine,
        sizeof(stateLine),
        "MESSAGE autoplay -> %s (maincmd=%u user=%u char=%u)",
        on ? "ON" : "OFF",
        static_cast<unsigned int>(mainCmd),
        pUserDataCtx->GetUserNum(),
        pUserDataCtx->GetCharacterIdx()
    );
    Management::WriteLogs(kLogPath, stateLine);

    return P_OK;
}
