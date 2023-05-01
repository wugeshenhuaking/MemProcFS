// m_fc_yara.c : implementation of YARA scanning forensic module.
//
// REQUIRE: FORENSIC SUB-SYSTEM INIT
//
// (c) Ulf Frisk, 2023
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "modules.h"
#include "../infodb.h"
#include "../vmmyarautil.h"

#define MFC_YARA_MAX_MATCHES    0x10000

typedef struct tdMFCYARA_CONTEXT {
    DWORD cMatches;
    POB_MEMFILE pmfObMemFileUser;
    PVMMYARAUTILOB_CONTEXT ctxObInit;
} MFCYARA_CONTEXT, *PMFCYARA_CONTEXT;

VOID MFcYara_IngestVirtmem(_In_ VMM_HANDLE H, _In_opt_ PVOID ctxfc, _In_ PVMMDLL_PLUGIN_FORENSIC_INGEST_VIRTMEM pIngestVirtmem)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxfc;
    VMMYARAUTIL_SCAN_CONTEXT ctxScan;
    if(!ctx || (VmmYaraUtil_MatchCount(ctx->ctxObInit) >= MFC_YARA_MAX_MATCHES)) { return; }
    ctxScan.ctx = ctx->ctxObInit;
    ctxScan.dwPID = pIngestVirtmem->dwPID;
    ctxScan.qwA = pIngestVirtmem->va;
    ctxScan.pb = pIngestVirtmem->pb;
    ctxScan.cb = pIngestVirtmem->cb;
    VmmYara_ScanMemory(
        VmmYaraUtil_Rules(ctx->ctxObInit),
        pIngestVirtmem->pb,
        pIngestVirtmem->cb,
        VMMYARA_SCAN_FLAGS_FAST_MODE | VMMYARA_SCAN_FLAGS_REPORT_RULES_MATCHING,
        (VMMYARA_SCAN_MEMORY_CALLBACK)VmmYaraUtil_MatchCB,
        &ctxScan,
        0
    );
}

/*
* Finalize ingestion of YARA results.
* This callback has the advantage that it is called before FcFinalize().
*/
VOID MFcYara_FcIngestFinalize(_In_ VMM_HANDLE H, _In_opt_ PVOID ctxfc)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxfc;
    LPSTR uszTXT, uszCSV;
    DWORD dwType;
    PVMM_PROCESS pObProcess = NULL;
    VMMYARAUTIL_PARSE_RESULT_FINDEVIL FindEvilResult;
    if(!ctx) { return; }
    if(!(VmmYaraUtil_IngestFinalize(H, ctx->ctxObInit))) { goto fail; }
    FcFileAppend(H, "yara.csv", VMMYARAUTIL_CSV_HEADER);
    while(VmmYaraUtil_ParseSingleResultNext(H, ctx->ctxObInit, &uszTXT, &uszCSV, &dwType, &FindEvilResult)) {
        if(0 == dwType) {
            // normal "yara module" match.
            ctx->cMatches++;
            ObMemFile_AppendString(ctx->pmfObMemFileUser, uszTXT);
        } else if(1 == dwType) {
            // findevil match - also log a findevil entry.
            ObMemFile_AppendString(H->fc->FindEvil.pmfYara, uszTXT);
            if(FindEvilResult.fValid) {
                pObProcess = VmmProcessGet(H, FindEvilResult.dwPID);
                FcEvilAdd(H, FindEvilResult.EvilType, pObProcess, FindEvilResult.va, "%s", FindEvilResult.uszText);
                Ob_DECREF_NULL(&pObProcess);
            }
        }
        FcFileAppend(H, "yara.csv", "%s", uszCSV);
    }
fail:
    Ob_DECREF_NULL(&ctx->ctxObInit);
}

PVOID MFcYara_FcInitialize(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxP->ctxM;
    VMMYARA_ERROR err;
    PVMMYARA_RULES pYrRules = NULL;
    PINFODB_YARA_RULES pObYaraRules = NULL;
    LPSTR szUserYaraRules = H->cfg.szForensicYaraRules;
    // 1: try initialize pre-compiled yara rules,
    //    compiled rules will disable built-in rules:
    if(szUserYaraRules[0]) {
        err = VmmYara_RulesLoadCompiled(szUserYaraRules, &pYrRules);
        if(err == VMMYARA_ERROR_SUCCESS) { goto finish; }
    }
    // 2: try initialize combined rules (built-in rules + optional user rule):
    if(InfoDB_YaraRulesBuiltIn(H, &pObYaraRules)) {
        if(szUserYaraRules[0]) {
            pObYaraRules->szRules[0] = szUserYaraRules;
            err = VmmYara_RulesLoadSourceCombined(pObYaraRules->cRules, pObYaraRules->szRules, &pYrRules);
        } else {
            err = VmmYara_RulesLoadSourceCombined(pObYaraRules->cRules - 1, pObYaraRules->szRules + 1, &pYrRules);
        }
        goto finish;
    }
    // 3: try initialize user rule:
    err = VmmYara_RulesLoadSourceFile(1, &szUserYaraRules, &pYrRules);
finish:
    Ob_DECREF(pObYaraRules);
    if(err != VMMYARA_ERROR_SUCCESS) {
        VMMDLL_Log(H, ctxP->MID, LOGLEVEL_2_WARNING, "yr_initialize() failed with error code %i", err);
        if(pYrRules) { VmmYara_RulesDestroy(pYrRules); }
        return NULL;
    }
    ctx->ctxObInit = VmmYaraUtil_Initialize(H, &pYrRules, MFC_YARA_MAX_MATCHES);
    if(!ctx->ctxObInit) {
        if(pYrRules) { VmmYara_RulesDestroy(pYrRules); }
        return NULL;
    }
    return ctx;
}

NTSTATUS MFcYara_Read(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxP->ctxM;
    if(CharUtil_StrEquals(ctxP->uszPath, "result.txt", TRUE)) {
        return ObMemFile_ReadFile(ctx->pmfObMemFileUser, pb, cb, pcbRead, cbOffset);
    }
    if(CharUtil_StrEquals(ctxP->uszPath, "rules.txt", TRUE)) {
        return Util_VfsReadFile_FromStrA(H->cfg.szForensicYaraRules, pb, cb, pcbRead, cbOffset);
    }
    if(CharUtil_StrEquals(ctxP->uszPath, "match-count.txt", TRUE)) {
        return Util_VfsReadFile_FromNumber(ctx->cMatches, pb, cb, pcbRead, cbOffset);
    }
    return VMMDLL_STATUS_FILE_INVALID;
}

BOOL MFcYara_List(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Inout_ PHANDLE pFileList)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxP->ctxM;
    if(ctxP->uszPath[0]) { return FALSE; }
    VMMDLL_VfsList_AddFile(pFileList, "match-count.txt", Util_GetNumDigits(ctx->cMatches), NULL);
    VMMDLL_VfsList_AddFile(pFileList, "result.txt", ObMemFile_Size(ctx->pmfObMemFileUser), NULL);
    VMMDLL_VfsList_AddFile(pFileList, "rules.txt", strlen(H->cfg.szForensicYaraRules), NULL);
    return TRUE;
}

VOID MFcYara_Close(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP)
{
    PMFCYARA_CONTEXT ctx = (PMFCYARA_CONTEXT)ctxP->ctxM;
    if(ctx) {
        Ob_DECREF(ctx->pmfObMemFileUser);
        Ob_DECREF(ctx->ctxObInit);
        LocalFree(ctx);
    }
}

BOOL MFcYara_ExistsRules(_In_ VMM_HANDLE H)
{
    if(H->cfg.fDisableYara) { return FALSE; }
    return H->cfg.szForensicYaraRules[0] || InfoDB_YaraRulesBuiltIn_Exists(H);
}

VOID MFcYara_Notify(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _In_ DWORD fEvent, _In_opt_ PVOID pvEvent, _In_opt_ DWORD cbEvent)
{
    if(fEvent == VMMDLL_PLUGIN_NOTIFY_FORENSIC_INIT_COMPLETE) {
        PluginManager_SetVisibility(H, TRUE, "\\forensic\\yara", TRUE);
    }
}

/*
* Initialize the forensic yara plugin. This plugin is responsible for yara
* scanning both with user supplied rules and with the built-in default rules
* used by the FindEvil plugin.
* Since scanning is quite heavy only initialize the plugin if there are rules
* to scan with.
*/
VOID M_FcYara_Initialize(_In_ VMM_HANDLE H, _Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    PMFCYARA_CONTEXT ctx = NULL;
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if(!MFcYara_ExistsRules(H)) { return; }
    if(!(ctx = LocalAlloc(LMEM_ZEROINIT, sizeof(MFCYARA_CONTEXT)))) { return; }
    if(!(ctx->pmfObMemFileUser = ObMemFile_New(H, H->vmm.pObCacheMapObCompressedShared))) { return; }
    pRI->reg_info.ctxM = (PVMMDLL_PLUGIN_INTERNAL_CONTEXT)ctx;
    strcpy_s(pRI->reg_info.uszPathName, 128, "\\forensic\\yara");
    pRI->reg_info.fRootModule = TRUE;
    pRI->reg_info.fRootModuleHidden = TRUE;
    pRI->reg_fn.pfnList = MFcYara_List;
    pRI->reg_fn.pfnRead = MFcYara_Read;
    pRI->reg_fn.pfnNotify = MFcYara_Notify;
    pRI->reg_fn.pfnClose = MFcYara_Close;
    pRI->reg_fnfc.pfnInitialize = MFcYara_FcInitialize;
    pRI->reg_fnfc.pfnIngestVirtmem = MFcYara_IngestVirtmem;
    pRI->reg_fnfc.pfnIngestFinalize = MFcYara_FcIngestFinalize;
    pRI->pfnPluginManager_Register(H, pRI);
}
