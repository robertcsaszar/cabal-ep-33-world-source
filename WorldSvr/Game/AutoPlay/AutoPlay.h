#pragma once
#include <Core.h>
#include <Game/Structs/GameStructs.h>
#include <map>

//
// Server-side AutoPlay (MVP - Faza 1a: auto-target + monitorizare HP).
//
// Design:
//  - NU cream un thread propriu. Tick-ul ruleaza in interiorul handler-ului de
//    CSC_HEARTBEAT, adica pe firul principal al serverului cand clientul
//    jucatorului trimite heartbeat periodic. Astfel accesul la structurile de
//    joc (lista de mobs, pozitia, HP) e serializat cu logica serverului =>
//    fara race conditions / crash-uri de concurenta.
//  - Starea este per-jucator (cheie: userNum).
//
// Ce face Faza 1a (complet functional, fara adrese noi din binar):
//  - la fiecare heartbeat al unui jucator cu autoplay ON:
//      * scaneaza mob-urile din lume in raza kScanRadius
//      * alege cel mai apropiat mob viu ca tinta
//      * citeste HP% curent
//      * scrie o linie de status in log (dovada ca loop-ul merge server-side)
//
// Faza 1b (atac/miscare) este schitata separat si dezactivata implicit
// (vezi AUTOPLAY_ENABLE_ACTIONS in AutoPlay.cpp) fiindca necesita confirmarea
// layout-ului exact al pachetului C2S_ATTCKTOMOBS pentru EP33.
//
class AutoPlay : public Singleton<AutoPlay>
{
public:
	struct State
	{
		bool enabled		= false;
		int  targetMobRow	= -1;
		long lastLogSec		= 0;
	};

	void Init();

	// API de control (leaga-l de trigger-ul tau: GM command / pachet custom).
	bool Toggle(DWORD userNum);
	void Set(DWORD userNum, bool on);
	bool IsEnabled(DWORD userNum);

	// Inregistrat pe CSC_HEARTBEAT; ruleaza un tick pentru jucatorul emitent.
	static int OnHeartbeat(int* pProcessLayer, PROCESSDATACONTEXT* pProcessDataCtx);

	// Inregistrat pe CSC_GMCOMMAND; detecteaza "/autoplay on|off" din payload.
	static int OnGMCommand(int* pProcessLayer, PROCESSDATACONTEXT* pProcessDataCtx);

private:
	void Tick(USERCONTEXT* pUserCtx, USERDATACONTEXT* pUserDataCtx);

	// Cauta un token ASCII (case-insensitive) intr-un buffer marginit. Folosit
	// pentru a parsa comanda fara a cunoaste layout-ul exact al pachetului GM.
	static bool PayloadContains(const char* buf, int len, const char* token);

	// Returneaza randul (row) celui mai apropiat mob viu in raza data, sau -1.
	// Distanta patratica minima gasita este pusa in *outDistSq (daca != null).
	int FindNearestMob(USERDATACONTEXT* pUserDataCtx, int radius, long long* outDistSq);

	std::map<DWORD, State> m_states;
};

extern AutoPlay* g_pAutoPlay;
