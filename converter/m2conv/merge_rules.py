# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Manual resolutions for Lua merges: module -> rules"""
RULES = {
 'dlc01_mrxguipda': {'main': 'theirs', 'prepend_import': [b'MrxGui']},
 'dlc_mrxguidialogbox': {'main': 'theirs'},
    # the DLC pause screen lacks the PC port's flash event handlers (options, PauseItemChanged ...): without them
    # the pause menu's settings are never loaded or reverted and every resume asks "Accept changes?"
    'mrxguipausescreen': {'graft': {b'_FinishPauseOpen': (b'SetFlashPauseMenu', b'SetFlashEventHandler')}},
    'mrxguipda': {'main': 'theirs', 'prepend_import': [b'MrxGui']},
    'mrxmultipagemenu': {'fn': {b'_ChooseOption': 'pc', b'_Display': 'pc'},
                         'kpatch': {b'_ChooseOption': {b'Previous page': b'[Patch2.GUI.PrevPage]', b'Next page': b'[Patch2.GUI.NextPage]'},
                                    b'_Display': {b'Previous page': b'[Patch2.GUI.PrevPage]', b'Next page': b'[Patch2.GUI.NextPage]'}}},
    'mrxguishell': {'fn': {b'HandleInitializationEvent': 'pc'}},
}
