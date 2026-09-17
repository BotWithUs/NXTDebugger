#pragma once
#include <cstddef>

#include "ipc/Events.h"
#include "ipc/SharedLayout.h"

// Compile-time pins for the absolute numbers wire/PROTOCOL.md prints.
//
// PROTOCOL.md is the normative, language-agnostic description of this wire — it
// is what a third-party consumer binds against, and it prints absolute byte
// offsets and struct sizes. Those numbers had nothing holding them to the
// headers. SharedLayout.h does assert its own layout, but *relationally*
// (`offsetof(Snapshot, npcs) == 28 + sizeof(LocalPlayer)`), which keeps the
// header self-consistent while saying nothing about the absolute figure the
// document publishes. A field could therefore be correct in C++ and wrong in
// the document, in the one place a consumer with no C++ compiler has to trust.
//
// Every assertion below is the number PROTOCOL.md prints, pinned to the real
// offsetof/sizeof. Change a layout and this file fails the build naming the
// section to update; change the document alone and the mismatch is still here
// to find. It compiles in a public clone with no producer checkout, because
// both sides of every comparison are in this repository.
//
// Included once, by wire/SnapshotReader.cpp — the module that decodes these
// offsets. Mirrors how src/attach/Session.h pins kProtocolVersion.
//
// Method and event *names* are checked separately, by tools/check_protocol_doc.ps1
// (wired into cmake.toml): names are not layout, so the compiler cannot see them.

namespace nxt::ipc
{

// --- PROTOCOL.md §2.2 geometry ---
static_assert(kProtocolVersion == 19);
static_assert(sizeof(SharedHeader) == 64);
static_assert(sizeof(Snapshot) == 365744);

// --- PROTOCOL.md §2.3 SharedHeader ---
static_assert(offsetof(SharedHeader, magic) == 0);
static_assert(offsetof(SharedHeader, version) == 4);
static_assert(offsetof(SharedHeader, headerSize) == 8);
static_assert(offsetof(SharedHeader, layoutId) == 12);
static_assert(offsetof(SharedHeader, snapshotSize) == 16);
static_assert(offsetof(SharedHeader, snapshotOff0) == 20);
static_assert(offsetof(SharedHeader, snapshotOff1) == 24);
static_assert(offsetof(SharedHeader, ringOff) == 28);
static_assert(offsetof(SharedHeader, ringSize) == 32);
static_assert(offsetof(SharedHeader, frontIdx) == 36);
static_assert(offsetof(SharedHeader, targetPid) == 40);

// --- PROTOCOL.md §2.5 Snapshot ---
static_assert(offsetof(Snapshot, publishSeq) == 0);
static_assert(offsetof(Snapshot, gameState) == 8);
static_assert(offsetof(Snapshot, ownIndex) == 12);
static_assert(offsetof(Snapshot, rootIfaceId) == 16);
static_assert(offsetof(Snapshot, serverTick) == 20);
static_assert(offsetof(Snapshot, self) == 24);
static_assert(offsetof(Snapshot, npcCount) == 576);
static_assert(offsetof(Snapshot, npcs) == 580);
static_assert(offsetof(Snapshot, playerCount) == 37444);
static_assert(offsetof(Snapshot, players) == 37448);
static_assert(offsetof(Snapshot, locationCount) == 94792);
static_assert(offsetof(Snapshot, locations) == 94796);
static_assert(offsetof(Snapshot, inventoryCount) == 258640);
static_assert(offsetof(Snapshot, inventories) == 258644);
static_assert(offsetof(Snapshot, invItemCount) == 258900);
static_assert(offsetof(Snapshot, invItems) == 258904);
static_assert(offsetof(Snapshot, producer) == 275288);
static_assert(offsetof(Snapshot, openIfaceCount) == 275320);
static_assert(offsetof(Snapshot, openIfaces) == 275324);
static_assert(offsetof(Snapshot, groundItemCount) == 275580);
static_assert(offsetof(Snapshot, groundItems) == 275584);
static_assert(offsetof(Snapshot, projectileCount) == 291968);
static_assert(offsetof(Snapshot, projectiles) == 291972);
static_assert(offsetof(Snapshot, gameCycle) == 300164);
static_assert(offsetof(Snapshot, dynRegion) == 300168);
static_assert(offsetof(Snapshot, dynChunkCount) == 300204);
static_assert(offsetof(Snapshot, dynChunks) == 300208);

// --- PROTOCOL.md §2.7 LocalPlayer ---
static_assert(sizeof(LocalPlayer) == 552);
static_assert(offsetof(LocalPlayer, serverIndex) == 0);
static_assert(offsetof(LocalPlayer, combatLevel) == 4);
static_assert(offsetof(LocalPlayer, tileX) == 8);
static_assert(offsetof(LocalPlayer, tileY) == 10);
static_assert(offsetof(LocalPlayer, plane) == 12);
static_assert(offsetof(LocalPlayer, flags) == 13);
static_assert(offsetof(LocalPlayer, followingIndex) == 14);
static_assert(offsetof(LocalPlayer, animationId) == 16);
static_assert(offsetof(LocalPlayer, stanceId) == 20);
static_assert(offsetof(LocalPlayer, targetIndex) == 24);
static_assert(offsetof(LocalPlayer, targetType) == 26);
static_assert(offsetof(LocalPlayer, isMember) == 27);
static_assert(offsetof(LocalPlayer, spotAnimId) == 28);
static_assert(offsetof(LocalPlayer, skillCount) == 36);
static_assert(offsetof(LocalPlayer, skills) == 40);

// --- PROTOCOL.md §2.8 entries ---
static_assert(sizeof(NpcEntry) == 36);
static_assert(offsetof(NpcEntry, serverIndex) == 0);
static_assert(offsetof(NpcEntry, typeId) == 4);
static_assert(offsetof(NpcEntry, tileX) == 8);
static_assert(offsetof(NpcEntry, tileY) == 10);
static_assert(offsetof(NpcEntry, plane) == 12);
static_assert(offsetof(NpcEntry, flags) == 13);
static_assert(offsetof(NpcEntry, followingIndex) == 14);
static_assert(offsetof(NpcEntry, animationId) == 16);
static_assert(offsetof(NpcEntry, stanceId) == 20);
static_assert(offsetof(NpcEntry, hp) == 24);
static_assert(offsetof(NpcEntry, maxHp) == 28);
static_assert(offsetof(NpcEntry, spotAnimId) == 32);
static_assert(sizeof(PlayerEntry) == 28);
static_assert(offsetof(PlayerEntry, serverIndex) == 0);
static_assert(offsetof(PlayerEntry, tileX) == 4);
static_assert(offsetof(PlayerEntry, tileY) == 6);
static_assert(offsetof(PlayerEntry, plane) == 8);
static_assert(offsetof(PlayerEntry, flags) == 9);
static_assert(offsetof(PlayerEntry, followingIndex) == 10);
static_assert(offsetof(PlayerEntry, animationId) == 12);
static_assert(offsetof(PlayerEntry, stanceId) == 16);
static_assert(offsetof(PlayerEntry, combatLevel) == 20);
static_assert(offsetof(PlayerEntry, spotAnimId) == 24);
static_assert(sizeof(LocationEntry) == 20);
static_assert(offsetof(LocationEntry, typeId) == 0);
static_assert(offsetof(LocationEntry, interactId) == 4);
static_assert(offsetof(LocationEntry, animationId) == 8);
static_assert(offsetof(LocationEntry, tileX) == 12);
static_assert(offsetof(LocationEntry, tileY) == 14);
static_assert(offsetof(LocationEntry, plane) == 16);
static_assert(offsetof(LocationEntry, shape) == 17);
static_assert(offsetof(LocationEntry, rotation) == 18);
static_assert(offsetof(LocationEntry, flags) == 19);
static_assert(sizeof(GroundItemEntry) == 16);
static_assert(offsetof(GroundItemEntry, itemId) == 0);
static_assert(offsetof(GroundItemEntry, quantity) == 4);
static_assert(offsetof(GroundItemEntry, tileX) == 8);
static_assert(offsetof(GroundItemEntry, tileY) == 10);
static_assert(offsetof(GroundItemEntry, plane) == 12);
static_assert(sizeof(ProjectileEntry) == 32);
static_assert(offsetof(ProjectileEntry, projectileId) == 0);
static_assert(offsetof(ProjectileEntry, startCycle) == 4);
static_assert(offsetof(ProjectileEntry, endCycle) == 8);
static_assert(offsetof(ProjectileEntry, sourceIndex) == 12);
static_assert(offsetof(ProjectileEntry, sourceType) == 14);
static_assert(offsetof(ProjectileEntry, targetIndex) == 16);
static_assert(offsetof(ProjectileEntry, targetType) == 18);
static_assert(offsetof(ProjectileEntry, startTileX) == 20);
static_assert(offsetof(ProjectileEntry, startTileY) == 22);
static_assert(offsetof(ProjectileEntry, endTileX) == 24);
static_assert(offsetof(ProjectileEntry, endTileY) == 26);
static_assert(offsetof(ProjectileEntry, plane) == 28);
static_assert(sizeof(InventoryHeader) == 8);
static_assert(offsetof(InventoryHeader, invId) == 0);
static_assert(offsetof(InventoryHeader, slotCount) == 4);
static_assert(offsetof(InventoryHeader, firstItemIdx) == 6);
static_assert(sizeof(InventoryItem) == 8);
static_assert(offsetof(InventoryItem, itemId) == 0);
static_assert(offsetof(InventoryItem, quantity) == 4);
static_assert(sizeof(SkillEntry) == 16);
static_assert(offsetof(SkillEntry, typeId) == 0);
static_assert(offsetof(SkillEntry, experience) == 4);
static_assert(offsetof(SkillEntry, actualLevel) == 8);
static_assert(offsetof(SkillEntry, boostedLevel) == 12);

// --- PROTOCOL.md §2.9 ProducerState / 2.10 DynamicRegion ---
static_assert(sizeof(ProducerState) == 32);
static_assert(sizeof(DynamicRegion) == 36);
static_assert(offsetof(DynamicRegion, isInstance) == 0);
static_assert(offsetof(DynamicRegion, truncated) == 1);
static_assert(offsetof(DynamicRegion, sceneMode) == 4);
static_assert(offsetof(DynamicRegion, originMapX) == 8);
static_assert(offsetof(DynamicRegion, originMapY) == 12);
static_assert(offsetof(DynamicRegion, maxMapX) == 16);
static_assert(offsetof(DynamicRegion, maxMapY) == 20);
static_assert(offsetof(DynamicRegion, gridW) == 24);
static_assert(offsetof(DynamicRegion, gridH) == 28);
static_assert(offsetof(DynamicRegion, requiredChunks) == 32);

// --- PROTOCOL.md §3.x event bodies ---
static_assert(sizeof(LoginStateChangeBody) == 8);
static_assert(sizeof(TickBody) == 4);
static_assert(sizeof(VarChangeBody) == 12);
static_assert(sizeof(VarbitChangeBody) == 12);
static_assert(sizeof(ObjVarChangeBody) == 20);
static_assert(sizeof(ActionExecutedBody) == 16);
static_assert(sizeof(WalkBody) == 8);
static_assert(sizeof(HitmarkBody) == 20);
static_assert(sizeof(HeadbarBody) == 16);
static_assert(sizeof(SpotAnimBody) == 20);
static_assert(sizeof(KeyInputBody) == 8);
static_assert(sizeof(TokenRefreshFiredBody) == 16);
static_assert(sizeof(TokenRefreshedBody) == 16);
static_assert(sizeof(BreakStartedBody) == 24);
static_assert(sizeof(RadioGroupSelectBody) == 20);
static_assert(sizeof(ChatMessageBody) == 112);

// --- PROTOCOL.md §2.2 derived region geometry ---
// The doc prints these as a table of absolute byte figures; the header derives
// them, so this is where the two are held equal.
static_assert(kMagic           == 0x5354584Eu);
static_assert(kSnapshotStride  == 365760);   // sizeof(Snapshot) padded to 64B
static_assert(kSnapshotOff0    == 64);
static_assert(kSnapshotOff1    == 365824);
static_assert(kRingOff         == 731584);
static_assert(kRingStride      == 131136);   // "Event ring size (padded)"
static_assert(kRegionSize      == 862720);   // "Total region size"

// --- PROTOCOL.md §3.1 event ring layout ---
static_assert(kEventRingSlots  == 1024);     // EventRing.slotCount
static_assert(kEventSlotSize   == 128);
static_assert(kEventBodyMax    == 112);
static_assert(sizeof(EventSlot) == 128);
static_assert(offsetof(EventSlot, seq)        == 0);
static_assert(offsetof(EventSlot, type)       == 8);
static_assert(offsetof(EventSlot, bodyLen)    == 12);
static_assert(offsetof(EventSlot, body)       == 16);
static_assert(offsetof(EventRing, head)         == 0);
static_assert(offsetof(EventRing, slotCount)    == 8);
static_assert(offsetof(EventRing, slotMask)     == 12);
static_assert(offsetof(EventRing, droppedCount) == 16);
static_assert(offsetof(EventRing, slots)        == 32);

}
