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
//   - pachet = 16 octeti (verificare iLen == 0x10 in handler)
//   - offset 0x0a: DWORD index tinta (folosit low-word, mascat &0xFFFF / &0x3FF)
//   - offset 0x0e: BYTE tip obiect  (comparat cu 5 in handler)
//   - offset 0x0f: BYTE flag (world-mob)
// Lasa 0 pana testezi Faza 1a; pune 1 pentru a activa atacul (ideal doar pe
// canalul de test WorldSvr_01_04).
#define AUTOPLAY_ENABLE_ACTIONS 0

namespace
{
	// Raza de scanare in unitatile de pozitie ale serverului (iPosXCur/iPosYCur).
	// Ajusteaza dupa scala lumii tale (tile vs. precise). 200 e un punct de start.
	constexpr int  kScanRadius   = 200;

	// Prag HP sub care Faza 1b ar folosi potiune (momentan doar raportat in log).
	constexpr int  kLowHpPercent = 30;

	const std::string kLogPath = "./AutoPlay.log";
}

void AutoPlay::Init()
{
	// Ne atasam la handler-ul de heartbeat al serverului. REGISTER_PROC adauga
	// (append) procedura noastra in lista existenta pentru [CSC_HEARTBEAT],
	// exact ca la Warp/Proc, deci nu suprascriem handler-ul de baza.
	REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::CSC_HEARTBEAT, OnHeartbeat);

	// Trigger in-joc: comanda GM "/autoplay on" / "/autoplay off".
	REGISTER_PROC(g_sUsrProcedureMap, MAINCMD_VALUE_EX::CSC_GMCOMMAND, OnGMCommand);
}

bool AutoPlay::PayloadContains(const char* buf, int len, const char* token)
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
			const char a = static_cast<char>(tolower(static_cast<unsigned char>(buf[i + k])));
			const char b = static_cast<char>(tolower(static_cast<unsigned char>(token[k])));
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
	return it != m_states.end() && it->second.enabled;
}

bool AutoPlay::Toggle(DWORD userNum)
{
	bool now = !IsEnabled(userNum);
	m_states[userNum].enabled = now;
	return now;
}

int AutoPlay::FindNearestMob(USERDATACONTEXT* pUserDataCtx, int radius, long long* outDistSq)
{
	CWorld* pWorld = pUserDataCtx->sPosData.pWorld;
	if (!pWorld)
		return -1;

	const int px = pUserDataCtx->sPosData.iPosXCur;
	const int py = pUserDataCtx->sPosData.iPosYCur;
	const long long radSq = static_cast<long long>(radius) * radius;

	int       bestRow = -1;
	long long bestSq  = radSq + 1;

	for (int row = 0; row < pWorld->iMobsCount; ++row)
	{
		MOBSCONTEXT* pMob = pWorld->GetMobPtr(row);
		if (!pMob)
			continue;
		if (pMob->bIsDead)
			continue;
		if (pMob->sParameters.iHP <= 0)
			continue;

		const long long dx = static_cast<long long>(pMob->sPosData.iPosXCur) - px;
		const long long dy = static_cast<long long>(pMob->sPosData.iPosYCur) - py;
		const long long d2 = dx * dx + dy * dy;

		if (d2 < bestSq)
		{
			bestSq  = d2;
			bestRow = row;
		}
	}

	if (outDistSq)
		*outDistSq = bestRow >= 0 ? bestSq : -1;

	return bestRow;
}

#if AUTOPLAY_ENABLE_ACTIONS
// Pachetul de atac pe mob (C2S_ATTCKTOMOBS), 16 octeti. Layout confirmat prin
// dezasamblarea binarului EP33 (vezi nota de la AUTOPLAY_ENABLE_ACTIONS).
#pragma pack(push, 1)
struct C2S_ATTCKTOMOBS_PKT
{
	WORD  wMagicCode;    // 0x00
	WORD  wPayLoadLen;   // 0x02
	DWORD dwCheckSum;    // 0x04
	WORD  wMainCmd;      // 0x08
	// payload (6 octeti):
	WORD  wTargetIdx;    // 0x0a  index obiect tinta (low-word folosit de server)
	WORD  wTargetIdxHi;  // 0x0c  (restul DWORD-ului citit la 0x0a; de regula 0)
	BYTE  bTargetType;   // 0x0e  tip obiect
	BYTE  bWorldMob;     // 0x0f  flag world-mob
};
#pragma pack(pop)
static_assert(sizeof(C2S_ATTCKTOMOBS_PKT) == 0x10, "attack packet must be 16 bytes");

// Handler nativ al serverului: int OnCSCAttckToMobs(processLayer, PROCESSDATACONTEXT*).
typedef int (*OnCSCAttckToMobs_t)(long long pProcessLayer, PROCESSDATACONTEXT* pCtx);
static OnCSCAttckToMobs_t OnCSCAttckToMobs = reinterpret_cast<OnCSCAttckToMobs_t>(0x007456C0);

// Trimite un atac catre mob reutilizand chiar handler-ul de atac al serverului
// (toata validarea/damage/EXP-ul nativ). Valorile de tip/flag sunt luate direct
// din objIdx al mob-ului, ca sa nu depindem de constante ghicite.
static void TryAttack(USERCONTEXT* pUserCtx, MOBSCONTEXT* pMob)
{
	C2S_ATTCKTOMOBS_PKT pkt = {};
	pkt.wMagicCode  = MAGIC_CODE;
	pkt.wPayLoadLen = sizeof(pkt);
	pkt.wMainCmd    = MAINCMD_VALUE_EX::CSC_ATTCKTOMOBS;
	pkt.wTargetIdx  = static_cast<WORD>(pMob->objIdx.sObjIdxData);
	pkt.wTargetIdxHi = 0;
	pkt.bTargetType = pMob->objIdx.objectType;
	pkt.bWorldMob   = pMob->objIdx.bWorldMob;

	PROCESSDATACONTEXT ctx = {};
	ctx.pUserCtx = reinterpret_cast<int*>(pUserCtx);
	ctx.cpPacket = reinterpret_cast<char*>(&pkt);
	ctx.iLen     = sizeof(pkt);

	OnCSCAttckToMobs(0, &ctx);
}
#endif

void AutoPlay::Tick(USERCONTEXT* pUserCtx, USERDATACONTEXT* pUserDataCtx)
{
	State& st = m_states[pUserDataCtx->GetUserNum()];

	long long distSq = -1;
	const int row = FindNearestMob(pUserDataCtx, kScanRadius, &distSq);
	st.targetMobRow = row;

	// HP curent (%)
	const long long hp    = pUserDataCtx->sParameters.iHP;
	const long long hpMax = pUserDataCtx->sParameters.iHPMax;
	const int hpPct = hpMax > 0 ? static_cast<int>((hp * 100) / hpMax) : 0;

	// Throttle log la ~1/secunda ca sa nu inundam fisierul la fiecare heartbeat.
	const long nowSec = static_cast<long>(time(nullptr));
	if (nowSec != st.lastLogSec)
	{
		st.lastLogSec = nowSec;

		const long long dist = distSq >= 0 ? static_cast<long long>(std::sqrt(static_cast<double>(distSq))) : -1;

		char line[256];
		snprintf(line, sizeof(line),
			"user=%u char=%u pos=(%d,%d) hp=%d%% nearestMobRow=%d dist=%lld lowHp=%d",
			pUserDataCtx->GetUserNum(),
			pUserDataCtx->GetCharacterIdx(),
			pUserDataCtx->sPosData.iPosXCur,
			pUserDataCtx->sPosData.iPosYCur,
			hpPct, row, dist, (hpPct <= kLowHpPercent) ? 1 : 0);

		Management::WriteLogs(kLogPath, line);
	}

#if AUTOPLAY_ENABLE_ACTIONS
	if (row >= 0)
	{
		if (MOBSCONTEXT* pMob = pUserDataCtx->sPosData.pWorld->GetMobPtr(row))
			TryAttack(pUserCtx, pMob);
	}
#endif
}

int AutoPlay::OnHeartbeat(int* pProcessLayer, PROCESSDATACONTEXT* pProcessDataCtx)
{
	ALIAS_PTR(USERCONTEXT, pUserCtx, pProcessDataCtx->pUserCtx);
	ALIAS_PTR(USERDATACONTEXT, pUserDataCtx, pUserCtx->pData);

	// Nu consumam / nu blocam heartbeat-ul: lasam procesarea normala sa continue.
	if (!pUserDataCtx->bIsActvteLink)
		return P_OK;

	// DIAG: dovedeste ca handler-ul de heartbeat e apelat si ca plugin-ul
	// primeste pachete (throttle ~5s, global). Chiar si cand autoplay e OFF.
	{
		static long lastDiag = 0;
		const long now = static_cast<long>(time(nullptr));
		if (now - lastDiag >= 5)
		{
			lastDiag = now;
			char l[128];
			snprintf(l, sizeof(l), "DIAG heartbeat OK user=%u autoplay=%d",
				pUserDataCtx->GetUserNum(),
				g_pAutoPlay->IsEnabled(pUserDataCtx->GetUserNum()) ? 1 : 0);
			Management::WriteLogs(kLogPath, l);
		}
	}

	if (g_pAutoPlay->IsEnabled(pUserDataCtx->GetUserNum()))
		g_pAutoPlay->Tick(pUserCtx, pUserDataCtx);

	return P_OK;
}

int AutoPlay::OnGMCommand(int* pProcessLayer, PROCESSDATACONTEXT* pProcessDataCtx)
{
    ALIAS_PTR(USERCONTEXT, pUserCtx, pProcessDataCtx->pUserCtx);
    ALIAS_PTR(USERDATACONTEXT, pUserDataCtx, pUserCtx->pData);

    const unsigned char* payload =
        reinterpret_cast<const unsigned char*>(pProcessDataCtx->cpPacket);

    const int len = static_cast<int>(pProcessDataCtx->iLen);

    // Diagnostic HEX
    {
        char hex[256];
        int pos = 0;

        for (int i = 0; i < len &&
             pos < static_cast<int>(sizeof(hex)) - 4; ++i)
        {
            pos += snprintf(
                hex + pos,
                sizeof(hex) - pos,
                "%02X ",
                payload[i]
            );
        }

        hex[pos] = 0;

        char l[320];
        snprintf(
            l,
            sizeof(l),
            "DIAG GMCMD primit len=%d hex=%s",
            len,
            hex
        );

        Management::WriteLogs(kLogPath, l);
    }

    if (!pUserDataCtx->bIsActvteLink)
        return P_OK;

    // Pachetul observat pentru /autoplay:
    //
    // 00-01 = magic
    // 02-03 = packet length
    // 04-07 = checksum
    // 08-09 = CSC_GMCOMMAND
    // 0A    = AutoPlay command id (1)
    // 0B    = 1 ON / 0 OFF
    //
    if (len < 12)
        return P_OK;

    const BYTE commandId = payload[10];
    const BYTE value     = payload[11];

    // Comanda AutoPlay observata
    if (commandId != 0x01)
        return P_OK;

    const bool on = (value != 0);

    g_pAutoPlay->Set(
        pUserDataCtx->GetUserNum(),
        on
    );

    char line[128];
    snprintf(
        line,
        sizeof(line),
        "GMCMD autoplay -> %s (user=%u char=%u cmd=%u value=%u)",
        on ? "ON" : "OFF",
        pUserDataCtx->GetUserNum(),
        pUserDataCtx->GetCharacterIdx(),
        commandId,
        value
    );

    Management::WriteLogs(kLogPath, line);

    return P_OK;
}
