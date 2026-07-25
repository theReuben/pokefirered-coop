Route22_Frlg_MapScripts::
	.byte 0

Route22_EventScript_EarlyRivalTriggerTop::
	lockall
	setvar VAR_TEMP_1, 0
	goto Route22_EventScript_EarlyRival
	end

Route22_EventScript_EarlyRivalTriggerMid::
	lockall
	setvar VAR_TEMP_1, 1
	setobjectxyperm LOCALID_ROUTE22_RIVAL, 25, 5
	goto Route22_EventScript_EarlyRival
	end

Route22_EventScript_EarlyRivalTriggerBottom::
	lockall
	setvar VAR_TEMP_1, 2
	setobjectxyperm LOCALID_ROUTE22_RIVAL, 25, 5
	goto Route22_EventScript_EarlyRival
	end

Route22_EventScript_EarlyRival::
	textcolor NPC_TEXT_COLOR_MALE
	playbgm MUS_RG_ENCOUNTER_RIVAL, 0
	addobject LOCALID_ROUTE22_RIVAL
	call_if_eq VAR_TEMP_1, 0, Route22_EventScript_EarlyRivalApproach
	call_if_eq VAR_TEMP_1, 1, Route22_EventScript_EarlyRivalApproach
	call_if_eq VAR_TEMP_1, 2, Route22_EventScript_EarlyRivalApproachBottom
	delay 6
	msgbox Route22_Text_EarlyRivalIntro
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 0, Route22_EventScript_EarlyRivalBattleDispatch
	special Multiplayer_BossReady_RivalRoute22_1
	message Route22_Text_WaitingForPartner
	waitmessage
	waitbossstart
	closemessage
Route22_EventScript_EarlyRivalBattleDispatch::
	@ Co-op: both ROMs MUST select the SAME rival team. VAR_STARTER_MON is the
	@ LOCAL player's starter and differs per instance, so keying the dispatch on
	@ it made each player build a different opponent party -> mid-battle desync.
	@ Use the shared remaining-starter key (mirrors the Oak's Lab dispatch).
	copyvar VAR_TEMP_0, VAR_STARTER_MON
	specialvar VAR_RESULT, Multiplayer_IsConnected
	call_if_eq VAR_RESULT, 1, EventScript_CoopRivalBattleKey
	call_if_eq VAR_TEMP_0, 2, Route22_EventScript_EarlyRivalSquirtle
	call_if_eq VAR_TEMP_0, 1, Route22_EventScript_EarlyRivalBulbasaur
	call_if_eq VAR_TEMP_0, 0, Route22_EventScript_EarlyRivalCharmander
	msgbox Route22_Text_EarlyRivalPostBattle
	closemessage
	delay 10
	playbgm MUS_RG_RIVAL_EXIT, 0
	call_if_eq VAR_TEMP_1, 0, Route22_EventScript_EarlyRivalExit
	call_if_eq VAR_TEMP_1, 1, Route22_EventScript_EarlyRivalExit
	call_if_eq VAR_TEMP_1, 2, Route22_EventScript_EarlyRivalExitBottom
	fadedefaultbgm
	removeobject LOCALID_ROUTE22_RIVAL
	setvar VAR_MAP_SCENE_ROUTE22, 2
	releaseall
	end

Route22_EventScript_EarlyRivalApproach::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_RivalApproach
	waitmovement 0
	return

Route22_EventScript_EarlyRivalApproachBottom::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_RivalApproachBottom
	applymovement LOCALID_PLAYER, Route22_Movement_PlayerFaceRival
	waitmovement 0
	return

Route22_EventScript_EarlyRivalSquirtle::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_EarlyRivalCoopSquirtle
	trainerbattle_earlyrival TRAINER_RIVAL_ROUTE22_EARLY_SQUIRTLE, 0, Route22_Text_EarlyRivalDefeat, Text_RivalVictory
	return
Route22_EventScript_EarlyRivalCoopSquirtle::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_EARLY_SQUIRTLE
	return

Route22_EventScript_EarlyRivalBulbasaur::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_EarlyRivalCoopBulbasaur
	trainerbattle_earlyrival TRAINER_RIVAL_ROUTE22_EARLY_BULBASAUR, 0, Route22_Text_EarlyRivalDefeat, Text_RivalVictory
	return
Route22_EventScript_EarlyRivalCoopBulbasaur::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_EARLY_BULBASAUR
	return

Route22_EventScript_EarlyRivalCharmander::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_EarlyRivalCoopCharmander
	trainerbattle_earlyrival TRAINER_RIVAL_ROUTE22_EARLY_CHARMANDER, 0, Route22_Text_EarlyRivalDefeat, Text_RivalVictory
	return
Route22_EventScript_EarlyRivalCoopCharmander::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_EARLY_CHARMANDER
	return

Route22_EventScript_EarlyRivalExit::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_EarlyRivalExit
	waitmovement 0
	return

Route22_EventScript_EarlyRivalExitBottom::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_EarlyRivalExitBottom
	waitmovement 0
	return

Route22_Movement_UnusedRivalExit:: @ 8
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	step_end

Route22_Movement_EarlyRivalExit::
	walk_down
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_down
	walk_down
	walk_down
	walk_down
	walk_down
	walk_down
	step_end

Route22_Movement_EarlyRivalExitBottom::
	walk_right
	walk_right
	walk_right
	walk_right
	walk_down
	walk_down
	walk_down
	walk_down
	walk_down
	walk_down
	walk_down
	step_end

Route22_Movement_RivalApproach::
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	step_end

Route22_Movement_RivalApproachBottom::
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_right
	walk_in_place_faster_down
	step_end

Route22_Movement_PlayerFaceRival::
	delay_16
	delay_16
	delay_16
	delay_16
	delay_16
	delay_16
	delay_16
	delay_8
	walk_in_place_faster_up
	step_end

Route22_EventScript_LateRivalTriggerTop::
	lockall
	setvar VAR_TEMP_1, 0
	goto Route22_EventScript_LateRival
	end

Route22_EventScript_LateRivalTriggerMid::
	lockall
	setvar VAR_TEMP_1, 1
	setobjectxyperm LOCALID_ROUTE22_RIVAL, 25, 5
	goto Route22_EventScript_LateRival
	end

Route22_EventScript_LateRivalTriggerBottom::
	lockall
	setvar VAR_TEMP_1, 2
	setobjectxyperm LOCALID_ROUTE22_RIVAL, 25, 5
	goto Route22_EventScript_LateRival
	end

Route22_EventScript_LateRival::
	textcolor NPC_TEXT_COLOR_MALE
	playbgm MUS_RG_ENCOUNTER_RIVAL, 0
	addobject LOCALID_ROUTE22_RIVAL
	call_if_eq VAR_TEMP_1, 0, Route22_EventScript_LateRivalApproach
	call_if_eq VAR_TEMP_1, 1, Route22_EventScript_LateRivalApproach
	call_if_eq VAR_TEMP_1, 2, Route22_EventScript_LateRivalApproachBottom
	msgbox Route22_Text_LateRivalIntro
	setvar VAR_LAST_TALKED, LOCALID_ROUTE22_RIVAL
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 0, Route22_EventScript_LateRivalBattleDispatch
	special Multiplayer_BossReady_RivalRoute22_2
	message Route22_Text_WaitingForPartner
	waitmessage
	waitbossstart
	closemessage
Route22_EventScript_LateRivalBattleDispatch::
	@ Co-op: same fix as the early rival dispatch above -- key the rival team on
	@ the shared remaining-starter key, not the per-instance VAR_STARTER_MON.
	copyvar VAR_TEMP_0, VAR_STARTER_MON
	specialvar VAR_RESULT, Multiplayer_IsConnected
	call_if_eq VAR_RESULT, 1, EventScript_CoopRivalBattleKey
	call_if_eq VAR_TEMP_0, 2, Route22_EventScript_LateRivalSquirtle
	call_if_eq VAR_TEMP_0, 1, Route22_EventScript_LateRivalBulbasaur
	call_if_eq VAR_TEMP_0, 0, Route22_EventScript_LateRivalCharmander
	msgbox Route22_Text_LateRivalPostBattle
	closemessage
	delay 10
	playbgm MUS_RG_RIVAL_EXIT, 0
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_LateRivalExit
	waitmovement 0
	fadedefaultbgm
	removeobject LOCALID_ROUTE22_RIVAL
	setvar VAR_MAP_SCENE_ROUTE22, 4
	releaseall
	end

Route22_EventScript_LateRivalApproach::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_RivalApproach
	waitmovement 0
	return

Route22_EventScript_LateRivalApproachBottom::
	applymovement LOCALID_ROUTE22_RIVAL, Route22_Movement_RivalApproachBottom
	applymovement LOCALID_PLAYER, Route22_Movement_PlayerFaceRival
	waitmovement 0
	return

Route22_EventScript_LateRivalSquirtle::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_LateRivalCoopSquirtle
	trainerbattle_no_intro TRAINER_RIVAL_ROUTE22_LATE_SQUIRTLE, Route22_Text_LateRivalDefeat
	return
Route22_EventScript_LateRivalCoopSquirtle::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_LATE_SQUIRTLE
	return

Route22_EventScript_LateRivalBulbasaur::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_LateRivalCoopBulbasaur
	trainerbattle_no_intro TRAINER_RIVAL_ROUTE22_LATE_BULBASAUR, Route22_Text_LateRivalDefeat
	return
Route22_EventScript_LateRivalCoopBulbasaur::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_LATE_BULBASAUR
	return

Route22_EventScript_LateRivalCharmander::
	specialvar VAR_RESULT, Multiplayer_IsConnected
	goto_if_eq VAR_RESULT, 1, Route22_EventScript_LateRivalCoopCharmander
	trainerbattle_no_intro TRAINER_RIVAL_ROUTE22_LATE_CHARMANDER, Route22_Text_LateRivalDefeat
	return
Route22_EventScript_LateRivalCoopCharmander::
	waitcoopparty
	waitpartysync
	docooptrainerbattle TRAINER_RIVAL_ROUTE22_LATE_CHARMANDER
	return

Route22_Movement_LateRivalExit::
	walk_left
	walk_left
	walk_left
	walk_left
	walk_left
	walk_left
	walk_left
	step_end

Route22_EventScript_LeagueGateSign::
	msgbox Route22_Text_LeagueGateSign, MSGBOX_SIGN
	end

Route22_Text_EarlyRivalIntro::
	.string "{RIVAL}: Hey! {PLAYER}!\p"
	.string "You're off to the POKéMON LEAGUE?\n"
	.string "Forget about it!\p"
	.string "You probably don't have any\n"
	.string "BADGES, do you?\p"
	.string "The guard won't let you through\n"
	.string "without them.\p"
	.string "By the way, did your POKéMON get\n"
	.string "any stronger?$"

Route22_Text_EarlyRivalDefeat::
	.string "Awww!\n"
	.string "You just lucked out!$"

Route22_Text_EarlyRivalPostBattle::
	.string "I heard the POKéMON LEAGUE is\n"
	.string "crawling with tough TRAINERS.\p"
	.string "I have to figure out how to get\n"
	.string "past them.\p"
	.string "You should quit dawdling and get a\n"
	.string "move on!$"

@ Translated in RB as "{RIVAL}: What? Why do I have 2 POKéMON? You should catch some more too!"
Route22_Text_RivalShouldCatchSomeMons::
	.string "{RIVAL}“なんだ？\n"
	.string "ポケモン　2ひきも\l"
	.string "もってるの　なぜか　だって？\p"
	.string "おまえも\n"
	.string "つかまえれば　いい　じゃん！$"

Route22_Text_LateRivalIntro::
	.string "{RIVAL}: What? {PLAYER}!\n"
	.string "What a surprise to see you here!\p"
	.string "So you're going to the POKéMON\n"
	.string "LEAGUE?\p"
	.string "You collected all the BADGES, too?\n"
	.string "That's cool!\p"
	.string "Then I'll whip you, {PLAYER}, as a\n"
	.string "warm-up for the POKéMON LEAGUE!\p"
	.string "Come on!$"

Route22_Text_LateRivalDefeat::
	.string "What!?\p"
	.string "I was just careless, you!$"

Route22_Text_LateRivalPostBattle::
	.string "That loosened me up.\n"
	.string "I'm ready for the POKéMON LEAGUE!\p"
	.string "{PLAYER}, you need more practice.\p"
	.string "But hey, you know that!\n"
	.string "I'm out of here. Smell ya!$"

@ Translated in RB as "{RIVAL}: Hahaha! {PLAYER}! That's your best? You're nowhere near as good as me, pal! Go train some more! You loser!"
Route22_Text_LateRivalVictory::
	.string "{RIVAL}“ひゃははッ　{PLAYER}ー！\n"
	.string "それで　がんばってるのかよ！\l"
	.string "おれの　さいのうに　くらべりゃ\l"
	.string "{PLAYER}は　まだまだ　だな！\p"
	.string "もっと　れんしゅう　こいよ！\n"
	.string "あははーッ！$"

Route22_Text_WaitingForPartner::
	.string "Waiting for\n"
	.string "partner...$"

Route22_Text_LeagueGateSign::
	.string "POKéMON LEAGUE\n"
	.string "Front Gate$"

