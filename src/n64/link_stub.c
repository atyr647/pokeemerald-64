/*
 * src/n64/link_stub.c
 *
 * N64 port — Phase 7: Link cable / RFU stub implementations.
 *
 * The N64 has no GBA link cable or wireless adapter.  All multiplayer
 * features are stubbed out so the single-player game compiles and runs
 * without modification.  Every function returns the "no link present"
 * value so existing callers silently skip multiplayer code paths.
 *
 * Replaces: src/link.c, src/link_rfu_2.c, src/link_rfu_3.c,
 *           src/librfu_rfu.c, src/librfu_intr.c, src/librfu_sio32id.c,
 *           src/librfu_stwi.c
 */

#include <string.h>
#include "global.h"
#include "link.h"
#include "link_rfu.h"
#include "multiboot.h"
#include "task.h"

/* LinkTestBGInfo struct is defined in src/link.c which is excluded from
 * the N64 build; provide the definition here. */
struct LinkTestBGInfo
{
    u32 screenBaseBlock;
    u32 paletteNum;
    u32 baseChar;
    u32 unused;
};

/* -----------------------------------------------------------------------
 * Global data required by link.h externs
 * --------------------------------------------------------------------- */
struct Link       gLink;
u8                gBlockSendBuffer[BLOCK_BUFFER_SIZE];
u16               gBlockRecvBuffer[MAX_RFU_PLAYERS][BLOCK_BUFFER_SIZE / 2];
u16               gLinkType             = 0;
u32               gLinkStatus           = 0;
struct LinkPlayer gLinkPlayers[MAX_RFU_PLAYERS];
bool8             gReceivedRemoteLinkPlayers = FALSE;
u32               gBerryBlenderKeySendAttempts = 0;
/* gLinkVSyncDisabled is defined in main.c as u8 gLinkVSyncDisabled */
u16               gLinkPartnersHeldKeys[6];
u32               gLinkDebugSeed        = 0;
struct LinkPlayerBlock gLocalLinkPlayerBlock;
bool8             gLinkErrorOccurred    = FALSE;
u32               gLinkDebugFlags       = 0;
bool8             gRemoteLinkPlayersNotReceived[MAX_LINK_PLAYERS];
u16               gLinkHeldKeys         = 0;
bool8             gReadyToCloseLink[MAX_LINK_PLAYERS];
u16               gReadyCloseLinkType   = 0;
u8                gSuppressLinkErrorMessage = 0;
bool8             gSavedLinkPlayerCount = FALSE;
struct LinkTestBGInfo gLinkTestBGInfo;
struct LinkPlayer gLocalLinkPlayer;

/* -----------------------------------------------------------------------
 * Global data required by link_rfu.h externs
 * --------------------------------------------------------------------- */
struct RfuGameData gHostRfuGameData;
u8                 gHostRfuUsername[PLAYER_NAME_LENGTH + 1];
struct RfuManager  gRfu;
u8                 gWirelessStatusIndicatorSpriteId = 0xFF;

/* -----------------------------------------------------------------------
 * link.h function stubs
 * --------------------------------------------------------------------- */
bool8  IsWirelessAdapterConnected(void)                    { return FALSE; }
void   OpenLink(void)                                      {}
void   CloseLink(void)                                     {}
u16    LinkMain2(const u16 *heldKeys)                      { (void)heldKeys; return 0; }
void   ClearLinkCallback(void)                             {}
void   ClearLinkCallback_2(void)                           {}
u8     GetLinkPlayerCount(void)                            { return 1; }
void   OpenLinkTimed(void)                                 {}
u8     GetLinkPlayerDataExchangeStatusTimed(int min, int max) { (void)min; (void)max; return 0; }
bool8  IsLinkPlayerDataExchangeComplete(void)              { return FALSE; }
u32    GetLinkPlayerTrainerId(u8 who)                      { (void)who; return 0; }
void   ResetLinkPlayers(void)                              {}
u8     GetMultiplayerId(void)                              { return 0; }
u8     BitmaskAllOtherLinkPlayers(void)                    { return 0; }
bool8  SendBlock(u8 unused, const void *src, u16 size)     { (void)unused; (void)src; (void)size; return FALSE; }
u8     GetBlockReceivedStatus(void)                        { return 0; }
void   ResetBlockReceivedFlags(void)                       {}
void   ResetBlockReceivedFlag(u8 who)                      { (void)who; }
u8     GetLinkPlayerCount_2(void)                          { return 1; }
bool8  IsLinkMaster(void)                                  { return FALSE; }
void   CB2_LinkError(void)                                 {}
bool8  GetSioMultiSI(void)                                 { return FALSE; }
bool8  IsLinkConnectionEstablished(void)                   { return FALSE; }
bool8  HasLinkErrorOccurred(void)                          { return FALSE; }
void   ResetSerial(void)                                   {}
u32    LinkMain1(u8 *adv, u16 *sendCmd, u16 (*recvCmds)[CMD_LENGTH])
                                                           { (void)adv; (void)sendCmd; (void)recvCmds; return 0; }
void   LinkVSync(void)                                     {}
/* Timer3Intr defined in src/n64/bios.c */
void   SerialCB(void)                                      {}
/* InUnionRoom defined in src/union_room.c */
void   LoadWirelessStatusIndicatorSpriteGfx(void)          {}
bool8  IsLinkTaskFinished(void)                            { return TRUE; }
void   CreateWirelessStatusIndicatorSprite(u8 x, u8 y)    { (void)x; (void)y; }
void   SetLinkStandbyCallback(void)                        {}
void   SetWirelessCommType1(void)                          {}
void   CheckShouldAdvanceLinkState(void)                   {}
void   SetCloseLinkCallback(void)                          {}
bool8  HandleLinkConnection(void)                          { return FALSE; }
void   SetLinkDebugValues(u32 seed, u32 flags)             { (void)seed; (void)flags; }
void   SetBerryBlenderLinkCallback(void)                   {}
void   SetSuppressLinkErrorMessage(bool8 flag)             { gSuppressLinkErrorMessage = flag; }
void   ConvertLinkPlayerName(struct LinkPlayer *player)    { (void)player; }
void   ClearSavedLinkPlayers(void)                         {}
void   SetLinkErrorBuffer(u32 status, u8 lastSend, u8 lastRecv, bool8 dc)
                                                           { (void)status; (void)lastSend; (void)lastRecv; (void)dc; }
void   LocalLinkPlayerToBlock(void)                        {}
void   LinkPlayerFromBlock(u32 who)                        { (void)who; }
bool32 Link_AnyPartnersPlayingFRLG_JP(void)                { return FALSE; }
void   ResetLinkPlayerCount(void)                          {}
void   SaveLinkPlayers(u8 playerCount)                     { (void)playerCount; }
void   SetWirelessCommType0(void)                          {}
bool32 IsLinkRecvQueueAtOverworldMax(void)                 { return FALSE; }
bool32 Link_AnyPartnersPlayingRubyOrSapphire(void)         { return FALSE; }
u32    LinkDummy_Return2(void)                             { return 2; }
void   SetLocalLinkPlayerId(u8 playerId)                   { (void)playerId; }
u8     GetSavedPlayerCount(void)                           { return 1; }
bool8  SendBlockRequest(u8 blockReqType)                   { (void)blockReqType; return FALSE; }
u8     GetLinkPlayerCountAsBitFlags(void)                  { return 1; }
u8     GetSavedLinkPlayerCountAsBitFlags(void)             { return 1; }
void   SetCloseLinkCallbackHandleJP(void)                  {}

/* Task_DestroySelf is declared in link.h but lives in task.c on GBA.
 * On N64 task.c is compiled normally, so we don't re-define it here. */

/* -----------------------------------------------------------------------
 * link_rfu.h function stubs
 * --------------------------------------------------------------------- */
void   WipeTrainerNameRecords(void)                        {}
void   InitRFUAPI(void)                                    {}
void   LinkRfu_Shutdown(void)                              {}
void   Rfu_SetBlockReceivedFlag(u8 id)                     { (void)id; }
void   Rfu_ResetBlockReceivedFlag(u8 id)                   { (void)id; }
bool32 IsSendingKeysToRfu(void)                            { return FALSE; }
void   StartSendingKeysToRfu(void)                         {}
void   Rfu_SetBerryBlenderLinkCallback(void)               {}
u8     Rfu_GetBlockReceivedStatus(void)                    { return 0; }
bool32 Rfu_InitBlockSend(const u8 *src, size_t size)       { (void)src; (void)size; return FALSE; }
void   ClearLinkRfuCallback(void)                          {}
u8     Rfu_GetLinkPlayerCount(void)                        { return 1; }
u8     Rfu_GetMultiplayerId(void)                          { return 0; }
bool8  Rfu_SendBlockRequest(u8 type)                       { (void)type; return FALSE; }
bool8  IsLinkRfuTaskFinished(void)                         { return TRUE; }
bool8  Rfu_IsMaster(void)                                  { return FALSE; }
void   Rfu_SetCloseLinkCallback(void)                      {}
void   Rfu_SetLinkStandbyCallback(void)                    {}
void   ResetLinkRfuGFLayer(void)                           {}
void   UpdateWirelessStatusIndicatorSprite(void)           {}
void   InitRFU(void)                                       {}
bool32 RfuMain1(void)                                      { return FALSE; }
bool32 RfuMain2(void)                                      { return FALSE; }
bool32 RfuHasErrored(void)                                 { return FALSE; }
bool32 IsRfuRecvQueueEmpty(void)                           { return TRUE; }
u32    GetRfuRecvQueueLength(void)                         { return 0; }
void   RfuVSync(void)                                      {}
void   RfuSetIgnoreError(bool32 enable)                    { (void)enable; }
u8     RfuGetStatus(void)                                  { return 0; }
void   UpdateGameData_GroupLockedIn(bool8 start)           { (void)start; }
void   RfuSetErrorParams(u32 info)                         { (void)info; }
void   RfuSetStatus(u8 status, u16 errInfo)                { (void)status; (void)errInfo; }
u8     Rfu_SetLinkRecovery(bool32 enable)                  { (void)enable; return 0; }
void   CopyHostRfuGameDataAndUsername(struct RfuGameData *gd, u8 *name)
                                                           { (void)gd; (void)name; }
void   SetHostRfuGameData(u8 act, u32 info, bool32 started){ (void)act; (void)info; (void)started; }
void   InitializeRfuLinkManager_LinkLeader(u32 max)        { (void)max; }
bool32 IsRfuCommunicatingWithAllChildren(void)             { return FALSE; }
void   LinkRfu_StopManagerAndFinalizeSlots(void)           {}
bool32 RfuTryDisconnectLeavingChildren(void)               { return FALSE; }
bool32 HasTrainerLeftPartnersList(u16 id, const u8 *name)  { (void)id; (void)name; return FALSE; }

/* -----------------------------------------------------------------------
 * Additional global data required by link / wireless code
 * --------------------------------------------------------------------- */
u8  gWirelessCommType          = 0;   /* 0 = cable, 1 = wireless */
u8  gShouldAdvanceLinkState    = 0;
u16 gSendCmd[CMD_LENGTH]       = {0};
u16 gRecvCmds[MAX_RFU_PLAYERS][CMD_LENGTH] = {{0}};

/* -----------------------------------------------------------------------
 * Additional link function stubs
 * --------------------------------------------------------------------- */
u8   GetWirelessCommType(void)              { return 0; }
void SetWirelessCommType(u8 type)           { (void)type; }
void ClearRecvCommands(void)                {}
void CheckLinkPlayersMatchSaved(void)       {}
void SaveLinkTrainerNames(void)             {}
void SetCloseLinkCallbackAndType(u16 type)  { (void)type; }
void StartSendingKeysToLink(void)           {}
void SetHostRfuWonderFlags(bool32 hasNews, bool32 hasCard) { (void)hasNews; (void)hasCard; }
void ResetHostRfuGameData(void)             {}
struct RfuGameData *GetHostRfuGameData(void) { return &gHostRfuGameData; }
void DestroyTask_RfuIdle(void)              {}
void InitializeRfuLinkManager_JoinGroup(void) {}
bool32 PlayerHasMetTrainerBefore(u16 id, u8 *name) { (void)id; (void)name; return FALSE; }
u32  GetLinkRecvQueueLength(void)           { return 0; }
/* IsSendingKeysOverCable defined in src/overworld.c */
void LinkRfu_FatalError(void)               {}

/* RFU additional stubs */
void rfu_REQ_stopMode(void)                 {}
bool32 Rfu_IsPlayerExchangeActive(void)     { return FALSE; }
void Rfu_DisconnectPlayerById(u32 playerIdx) { (void)playerIdx; }
void Rfu_StopPartnerSearch(void)            {}
void RfuSetNormalDisconnectMode(void)       {}
void SetUnionRoomChatPlayerData(u32 numPlayers) { (void)numPlayers; }
void SetTradeBoardRegisteredMonInfo(u32 type, u32 species, u32 level) { (void)type; (void)species; (void)level; }
void DestroyWirelessStatusIndicatorSprite(void) {}

/* -----------------------------------------------------------------------
 * GameCube multiboot stubs
 * --------------------------------------------------------------------- */
#include "libgcnmultiboot.h"
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Main(struct GcmbStruct *p)   { (void)p; }
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Init(struct GcmbStruct *p)   { (void)p; }
void GameCubeMultiBoot_Quit(void)                   {}

/* -----------------------------------------------------------------------
 * Additional missing link stubs
 * --------------------------------------------------------------------- */
bool32 IsSendingKeysToLink(void)                    { return FALSE; }
bool8  DoesLinkPlayerCountMatchSaved(void)          { return FALSE; }
u8     GetLinkPlayerInfoFlags(s32 playerId)         { (void)playerId; return 0; }
void   GetOtherPlayersInfoFlags(void)               {}
void   Task_DestroySelf(u8 taskId)                  { (void)taskId; }

/* -----------------------------------------------------------------------
 * RFU link manager stubs (AgbRfu_LinkManager excluded from N64 build)
 * --------------------------------------------------------------------- */
void   InitializeRfuLinkManager_EnterUnionRoom(void) {}
void   TryConnectToUnionRoomParent(const u8 *name, struct RfuGameData *parent, u8 activity)
                                                    { (void)name; (void)parent; (void)activity; }
bool32 IsUnionRoomListenTaskActive(void)            { return FALSE; }
void   SendLeaveGroupNotice(void)                   {}
void   StopUnionRoomLinkManager(void)               {}
void   LinkRfu_CreateConnectionAsParent(void)       {}
void   LinkRfu_StopManagerBeforeEnteringChat(void)  {}
bool8  LmanAcceptSlotFlagIsNotZero(void)            { return FALSE; }
void   UpdateGameData_SetActivity(u8 activity, u32 partnerInfo, bool32 startedActivity)
                                                    { (void)activity; (void)partnerInfo; (void)startedActivity; }
void   SendRfuStatusToPartner(u8 status, u16 trainerId, const u8 *name)
                                                    { (void)status; (void)trainerId; (void)name; }
u32    WaitSendRfuStatusToPartner(u16 trainerId, const u8 *name)
                                                    { (void)trainerId; (void)name; return 0; }
void   RequestDisconnectSlotByTrainerNameAndId(const u8 *name, u16 id)
                                                    { (void)name; (void)id; }
bool32 WaitRfuState(bool32 force)                   { (void)force; return FALSE; }
void   CreateTask_RfuIdle(void)                     {}
void   CreateTask_RfuReconnectWithParent(const u8 *name, u16 trainerId)
                                                    { (void)name; (void)trainerId; }
void   Rfu_SendPacket(void *data)                   { (void)data; }
bool8  Rfu_GetCompatiblePlayerData(struct RfuGameData *gameData, u8 *username, u8 idx)
                                                    { (void)gameData; (void)username; (void)idx; return FALSE; }
bool8  Rfu_GetWonderDistributorPlayerData(struct RfuGameData *gameData, u8 *username, u8 idx)
                                                    { (void)gameData; (void)username; (void)idx; return FALSE; }
s32    Rfu_GetIndexOfNewestChild(u8 bits)           { (void)bits; return -1; }

/* -----------------------------------------------------------------------
 * librfu hardware stubs (librfu_rfu.c etc excluded from N64 build)
 * All rfu_* functions are hardware-specific; stub as no-ops / errors.
 * --------------------------------------------------------------------- */
#include "librfu.h"

/* Global RFU state pointers — NULL means no hardware */
struct RfuLinkStatus    *gRfuLinkStatus               = NULL;
struct RfuSlotStatusNI  *gRfuSlotStatusNI[RFU_CHILD_MAX] = {NULL};
struct RfuSlotStatusUNI *gRfuSlotStatusUNI[RFU_CHILD_MAX] = {NULL};

u16   rfu_initializeAPI(u32 *b, u16 sz, IntrFunc *t, bool8 cr) { (void)b;(void)sz;(void)t;(void)cr; return 0; }
void  rfu_setTimerInterrupt(u8 n, IntrFunc *t)       { (void)n; (void)t; }
u16   rfu_syncVBlank(void)                           { return 0; }
void  rfu_setREQCallback(void (*cb)(u16,u16))        { (void)cb; }
u16   rfu_waitREQComplete(void)                      { return 0; }
u32   rfu_REQBN_softReset_and_checkID(void)          { return 0xFFFFFFFF; }
void  rfu_REQ_reset(void)                            {}
void  rfu_REQ_configSystem(u16 a, u8 b, u8 c)       { (void)a;(void)b;(void)c; }
void  rfu_REQ_configGameData(u8 f, u16 s, const u8 *g, const u8 *u) { (void)f;(void)s;(void)g;(void)u; }
void  rfu_REQ_startSearchChild(void)                 {}
void  rfu_REQ_pollSearchChild(void)                  {}
void  rfu_REQ_endSearchChild(void)                   {}
void  rfu_REQ_startSearchParent(void)                {}
void  rfu_REQ_pollSearchParent(void)                 {}
void  rfu_REQ_endSearchParent(void)                  {}
void  rfu_REQ_startConnectParent(u16 pid)            { (void)pid; }
void  rfu_REQ_pollConnectParent(void)                {}
void  rfu_REQ_endConnectParent(void)                 {}
u16   rfu_getConnectParentStatus(u8 *s, u8 *n)      { (void)s;(void)n; return 0; }
void  rfu_REQ_CHILD_startConnectRecovery(u8 b)       { (void)b; }
void  rfu_REQ_CHILD_pollConnectRecovery(void)        {}
void  rfu_REQ_CHILD_endConnectRecovery(void)         {}
u16   rfu_CHILD_getConnectRecoveryStatus(u8 *s)      { (void)s; return 0; }
u16   rfu_REQBN_watchLink(u16 id, u8 *bm, u8 *r, u8 *pbm)
                                                     { (void)id;(void)bm;(void)r;(void)pbm; return 0; }
void  rfu_REQ_disconnect(u8 bm)                      { (void)bm; }
void  rfu_REQ_changeMasterSlave(void)                {}
bool8 rfu_getMasterSlave(void)                       { return FALSE; }
void  rfu_setMSCCallback(void (*cb)(u16))            { (void)cb; }
void  rfu_clearAllSlot(void)                         {}
u16   rfu_clearSlot(u8 f, u8 idx)                   { (void)f;(void)idx; return 0; }
u16   rfu_setRecvBuffer(u8 t, u8 n, void *b, u32 sz){ (void)t;(void)n;(void)b;(void)sz; return 0; }
u16   rfu_UNI_setSendData(u8 bm, const void *s, u8 sz) { (void)bm;(void)s;(void)sz; return 0; }
void  rfu_UNI_readySendData(u8 idx)                  { (void)idx; }
u16   rfu_UNI_changeAndReadySendData(u8 i, const void *s, u8 sz) { (void)i;(void)s;(void)sz; return 0; }
u16   rfu_UNI_PARENT_getDRAC_ACK(u8 *f)             { (void)f; return 0; }
void  rfu_UNI_clearRecvNewDataFlag(u8 idx)           { (void)idx; }
u16   rfu_NI_setSendData(u8 bm, u8 sub, const void *s, u32 sz) { (void)bm;(void)sub;(void)s;(void)sz; return 0; }
u16   rfu_NI_CHILD_setSendGameName(u8 n, u8 sub)    { (void)n;(void)sub; return 0; }
u16   rfu_NI_stopReceivingData(u8 idx)               { (void)idx; return 0; }
u16   rfu_changeSendTarget(u8 t, u8 idx, u8 bm)     { (void)t;(void)idx;(void)bm; return 0; }
void  rfu_REQ_sendData(bool8 clk)                    { (void)clk; }
void  rfu_REQ_RFUStatus(void)                        {}
u16   rfu_getRFUStatus(u8 *s)                        { (void)s; return 0; }
u8   *rfu_getSTWIRecvBuffer(void)                    { return NULL; }
u16   rfu_REQBN_watchLink_slave(u16 id, u8 *bm, u8 *r, u8 *pbm)
                                                     { (void)id;(void)bm;(void)r;(void)pbm; return 0; }

/* -----------------------------------------------------------------------
 * ROM header stubs — RomHeaderGameCode / RomHeaderSoftwareVersion
 * (src/rom_header.s is the GBA version, excluded; provide N64 equivalents)
 * --------------------------------------------------------------------- */
const u8 RomHeaderGameCode[4]   = { 'B', 'P', 'E', 'E' };
const u8 RomHeaderSoftwareVersion = 0;

/* -----------------------------------------------------------------------
 * multiboot.h function stubs (multiboot.c excluded — ARM-only assembly)
 * --------------------------------------------------------------------- */
void MultiBootInit(struct MultiBootParam *mp)                                         { (void)mp; }
int  MultiBootMain(struct MultiBootParam *mp)                                         { (void)mp; return 0; }
void MultiBootStartProbe(struct MultiBootParam *mp)                                   { (void)mp; }
void MultiBootStartMaster(struct MultiBootParam *mp, const u8 *srcp, int length,
                          u8 palette_color, s8 palette_speed)
     { (void)mp; (void)srcp; (void)length; (void)palette_color; (void)palette_speed; }
int  MultiBootCheckComplete(struct MultiBootParam *mp)                                { (void)mp; return 1; }
