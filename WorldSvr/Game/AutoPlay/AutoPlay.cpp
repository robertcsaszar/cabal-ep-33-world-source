#include "AutoPlay.h"
#include "Common/LibUtils.h"
#include "Game/Management/Management.h"
#include <cmath>
#include <ctime>
#include <cstring>
#include <cctype>

extern AutoPlay* g_pAutoPlay = AutoPlay::GetInstance();

// Comuta pe 1 doar dupa ce confirmi layout-ul pachetului C2S_ATTCKTOMOBS
// pentru EP33 (vezi TryAttack de mai jos). Implicit dezactivat => build sigur.
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
// Faza 1b (schita): trimite personajul sa atace mob-ul tinta reutilizand
// handler-ul de atac deja inregistrat de server pentru [CSC_ATTCKTOMOBS],
// fara adrese noi din binar. Necesita:
//   1) layout-ul confirmat al pachetului C2S_ATTCKTOMOBS pentru EP33;
//   2) un accesor pe PROCEDUREMAP care sa ruleze procedurile inregistrate
//      (vezi nota din raspuns: PROCEDUREMAP::Execute).
static void TryAttack(USERCONTEXT* pUserCtx, MOBSCONTEXT* pMob)
{
	// #pragma pack(1)
	// struct C2S_ATTCKTOMOBS { C2S_HEADER hdr; OBJIDXDATA2 target; ... };
	// Construieste pachetul cu target = pMob->objIdx, apoi:
	//   PROCESSDATACONTEXT ctx{};
	//   ctx.pUserCtx = (int*)pUserCtx;
	//   ctx.cpPacket = (char*)&pkt;
	//   ctx.iLen     = sizeof(pkt);
	//   g_sUsrProcedureMap[MAINCMD_VALUE_EX::CSC_ATTCKTOMOBS]->Execute(0, &ctx);
	(void)pUserCtx; (void)pMob;
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

	if (g_pAutoPlay->IsEnabled(pUserDataCtx->GetUserNum()))
		g_pAutoPlay->Tick(pUserCtx, pUserDataCtx);

	return P_OK;
}

int AutoPlay::OnGMCommand(int* pProcessLayer, PROCESSDATACONTEXT* pProcessDataCtx)
{
	ALIAS_PTR(USERCONTEXT, pUserCtx, pProcessDataCtx->pUserCtx);
	ALIAS_PTR(USERDATACONTEXT, pUserDataCtx, pUserCtx->pData);

	if (!pUserDataCtx->bIsActvteLink)
		return P_OK;

	const char* payload = pProcessDataCtx->cpPacket;
	const int   len     = static_cast<int>(pProcessDataCtx->iLen);

	// Layout-agnostic: daca payload-ul nu contine "autoplay", nu e comanda
	// noastra => lasam procesarea GM normala sa continue (return P_OK).
	if (!PayloadContains(payload, len, "autoplay"))
		return P_OK;

	// "autoplay off" => OFF; altfel (inclusiv "autoplay on") => ON.
	const bool on = !PayloadContains(payload, len, "off");
	g_pAutoPlay->Set(pUserDataCtx->GetUserNum(), on);

	char line[128];
	snprintf(line, sizeof(line), "GMCMD autoplay -> %s (user=%u char=%u)",
		on ? "ON" : "OFF",
		pUserDataCtx->GetUserNum(),
		pUserDataCtx->GetCharacterIdx());
	Management::WriteLogs(kLogPath, line);

	// Feedback vizibil pe client: momentan doar in log. Pentru un mesaj de
	// sistem cu text ai nevoie de structura S2C de system-message EP33; se
	// poate adauga ulterior (sau un ACK crud via pUserCtx->SendErrorCode(...)).

	// Returnam P_OK ca sa nu deconectam. Daca vrei sa "consumi" comanda si sa
	// nu ajunga la handler-ul GM de baza (ex. mesaj "unknown command"), poti
	// schimba in P_FAIL dupa ce confirmi ca serverul trateaza asta ca "handled".
	return P_OK;
}
