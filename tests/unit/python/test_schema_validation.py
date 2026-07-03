"""Discord-rule command schema validation (T4.1).

These tests drive ``discord_modules.validate_command_schema`` and its wiring
into ``validate_module_manifests`` (the aggregation path used by
``register-discord-commands.py --validate-only``). Each rule group has one
accepting fixture and one rejecting fixture; rejecting cases assert on the
specific error-message substring so a regression in one rule cannot be masked
by another.
"""

import pytest

import discord_modules
from discord_modules import (
    DISCORD_LOCALES,
    ModuleError,
    command_count_by_module,
    validate_command_schema,
    validate_module_manifests,
)


def minimal_manifest(name="demo"):
    return {"name": name, "commands": "discord.commands.json"}


def install(make_module, commands, name="demo"):
    """Install a module whose only interesting content is its command schema."""
    return make_module(name, minimal_manifest(name), commands=commands)


def problems_for(commands, name="demo"):
    """Run schema validation and return the joined message string, or ''."""
    return "\n".join(validate_command_schema(commands, name))


# A canonical, fully valid CHAT_INPUT command reused by accepting cases.
def valid_chat_command():
    return {
        "name": "weather",
        "description": "Show the weather",
        "options": [
            {"type": 3, "name": "city", "description": "City name", "required": True},
            {
                "type": 3,
                "name": "unit",
                "description": "Temperature unit",
                "required": False,
                "choices": [
                    {"name": "Celsius", "value": "c"},
                    {"name": "Fahrenheit", "value": "f"},
                ],
            },
        ],
    }


# ---------------------------------------------------------------------------
# Wiring: schema validation surfaces through validate_module_manifests
# ---------------------------------------------------------------------------


class TestWiring:
    def test_valid_schema_passes_through_manifest_validation(self, repo_root, make_module):
        install(make_module, [valid_chat_command()])
        validate_module_manifests(repo_root)  # must not raise

    def test_invalid_schema_raises_module_error_through_aggregation(
        self, repo_root, make_module
    ):
        install(make_module, [{"name": "Bad", "description": "x"}])
        with pytest.raises(ModuleError, match="must be lowercase"):
            validate_module_manifests(repo_root)

    def test_absent_commands_file_is_not_validated(self, repo_root, make_module):
        # Characterization guard: a manifest that names a commands file which is
        # not on disk must still validate cleanly (matches existing behavior).
        make_module("demo", minimal_manifest("demo"))
        validate_module_manifests(repo_root)  # must not raise


# ---------------------------------------------------------------------------
# CHAT_INPUT name: charset + length
# ---------------------------------------------------------------------------


class TestChatInputNameCharset:
    def test_accepts_lowercase_letters_digits_dash_underscore(self):
        assert problems_for([{"name": "roll-2_d20", "description": "d"}]) == ""

    def test_rejects_invalid_character(self):
        msg = problems_for([{"name": "bad$name", "description": "d"}])
        assert "using only letters" in msg

    def test_rejects_overlong_name(self):
        msg = problems_for([{"name": "a" * 33, "description": "d"}])
        assert "using only letters" in msg

    def test_accepts_unicode_letter(self):
        # Unicode-aware approximation of \p{L}: category L* accepted.
        assert problems_for([{"name": "café", "description": "d"}]) == ""

    def test_accepts_devanagari_with_combining_mark(self):
        # Fix 4: Discord's pattern includes \p{sc=Deva}, which pulls in combining
        # marks (category M). "नाम" contains U+093E (category Mc); the Devanagari
        # block approximation must accept it.
        assert problems_for([{"name": "नाम", "description": "d"}]) == ""

    def test_accepts_thai_with_combining_mark(self):
        # Fix 4: "กั" is U+0E01 (Lo) + U+0E31 (Mn, a Thai combining mark). The
        # Thai block approximation must accept the combining mark.
        assert problems_for([{"name": "กั", "description": "d"}]) == ""


# ---------------------------------------------------------------------------
# CHAT_INPUT name: lowercase
# ---------------------------------------------------------------------------


class TestChatInputNameLowercase:
    def test_accepts_lowercase(self):
        assert problems_for([{"name": "ping", "description": "d"}]) == ""

    def test_rejects_uppercase(self):
        msg = problems_for([{"name": "Ping", "description": "d"}])
        assert "must be lowercase" in msg


# ---------------------------------------------------------------------------
# Descriptions (CHAT_INPUT commands + options)
# ---------------------------------------------------------------------------


class TestDescriptions:
    def test_accepts_valid_description(self):
        assert problems_for([{"name": "ping", "description": "Ping the bot"}]) == ""

    def test_rejects_missing_command_description(self):
        msg = problems_for([{"name": "ping"}])
        assert "description must be 1-100 characters" in msg

    def test_rejects_overlong_command_description(self):
        msg = problems_for([{"name": "ping", "description": "x" * 101}])
        assert "description must be 1-100 characters" in msg

    def test_rejects_missing_option_description(self):
        msg = problems_for(
            [{"name": "ping", "description": "d", "options": [{"type": 3, "name": "q"}]}]
        )
        assert "description must be 1-100 characters" in msg


# ---------------------------------------------------------------------------
# Context menu commands (type 2 / 3): no description, no options
# ---------------------------------------------------------------------------


class TestContextMenuCommands:
    def test_accepts_bare_user_command(self):
        assert problems_for([{"name": "Report User", "type": 2}]) == ""

    def test_accepts_bare_message_command(self):
        assert problems_for([{"name": "Pin Message", "type": 3}]) == ""

    def test_rejects_description_on_user_command(self):
        msg = problems_for([{"name": "Report User", "type": 2, "description": "x"}])
        assert "must not have a description" in msg

    def test_rejects_options_on_message_command(self):
        msg = problems_for(
            [{"name": "Pin", "type": 3, "options": [{"type": 3, "name": "x", "description": "d"}]}]
        )
        assert "must not have options" in msg


# ---------------------------------------------------------------------------
# Top-level command type: absent = chat-input; present must be int in {1,2,3}
# ---------------------------------------------------------------------------


class TestCommandType:
    def test_accepts_absent_type_as_chat_input(self):
        # Discord defaults an absent type to CHAT_INPUT (1).
        assert problems_for([{"name": "ping", "description": "d"}]) == ""

    def test_accepts_explicit_type_1(self):
        assert problems_for([{"name": "ping", "type": 1, "description": "d"}]) == ""

    def test_accepts_type_2_and_3(self):
        assert problems_for([{"name": "Report User", "type": 2}]) == ""
        assert problems_for([{"name": "Pin Message", "type": 3}]) == ""

    def test_rejects_unsupported_integer_type(self):
        msg = problems_for([{"name": "cmd", "type": 4, "description": "d"}])
        assert "unsupported command type" in msg

    def test_rejects_non_integer_type(self):
        msg = problems_for([{"name": "cmd", "type": "1", "description": "d"}])
        assert "command type must be an integer" in msg

    def test_rejects_boolean_type(self):
        # bool is an int subclass in Python; it must not be accepted as type 1.
        msg = problems_for([{"name": "cmd", "type": True, "description": "d"}])
        assert "command type must be an integer" in msg


# ---------------------------------------------------------------------------
# Option names: valid + unique per level
# ---------------------------------------------------------------------------


class TestOptionNames:
    def test_accepts_distinct_valid_option_names(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "first", "description": "d"},
                {"type": 3, "name": "second", "description": "d"},
            ],
        }
        assert problems_for([cmd]) == ""

    def test_rejects_invalid_option_name(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [{"type": 3, "name": "Bad Name", "description": "d"}],
        }
        msg = problems_for([cmd])
        assert "option name" in msg

    def test_rejects_duplicate_option_names_at_same_level(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "dup", "description": "d"},
                {"type": 4, "name": "dup", "description": "d"},
            ],
        }
        msg = problems_for([cmd])
        assert "duplicate option name" in msg


# ---------------------------------------------------------------------------
# Required options precede optional ones
# ---------------------------------------------------------------------------


class TestRequiredOrdering:
    def test_accepts_required_before_optional(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "req", "description": "d", "required": True},
                {"type": 3, "name": "opt", "description": "d", "required": False},
            ],
        }
        assert problems_for([cmd]) == ""

    def test_rejects_required_after_optional(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "opt", "description": "d", "required": False},
                {"type": 3, "name": "req", "description": "d", "required": True},
            ],
        }
        msg = problems_for([cmd])
        assert "must come before optional options" in msg


# ---------------------------------------------------------------------------
# <= 25 options per level
# ---------------------------------------------------------------------------


class TestOptionCount:
    def test_accepts_25_options(self):
        opts = [
            {"type": 3, "name": f"o{i}", "description": "d", "required": False}
            for i in range(25)
        ]
        assert problems_for([{"name": "cmd", "description": "d", "options": opts}]) == ""

    def test_rejects_26_options(self):
        opts = [
            {"type": 3, "name": f"o{i}", "description": "d", "required": False}
            for i in range(26)
        ]
        msg = problems_for([{"name": "cmd", "description": "d", "options": opts}])
        assert "options (maximum 25)" in msg


# ---------------------------------------------------------------------------
# Option type must be a supported integer (Finding 3)
# ---------------------------------------------------------------------------


class TestOptionType:
    def test_accepts_all_valid_option_types(self):
        # Discord option types 1..11 are all valid leaves/containers.
        for otype in range(1, 12):
            msg = problems_for(
                [
                    {
                        "name": "cmd",
                        "description": "d",
                        "options": [{"type": otype, "name": "opt", "description": "d"}],
                    }
                ]
            )
            assert "missing a type" not in msg and "unsupported type" not in msg, (
                otype,
                msg,
            )

    def test_rejects_unsupported_option_type(self):
        msg = problems_for(
            [
                {
                    "name": "cmd",
                    "description": "d",
                    "options": [{"type": 99, "name": "opt", "description": "d"}],
                }
            ]
        )
        assert "unsupported type" in msg

    def test_rejects_missing_option_type(self):
        msg = problems_for(
            [
                {
                    "name": "cmd",
                    "description": "d",
                    "options": [{"name": "opt", "description": "d"}],
                }
            ]
        )
        assert "missing a type" in msg


# ---------------------------------------------------------------------------
# Choices: count, name length, string value length
# ---------------------------------------------------------------------------


class TestChoices:
    def _cmd(self, choices):
        return {
            "name": "cmd",
            "description": "d",
            "options": [{"type": 3, "name": "o", "description": "d", "choices": choices}],
        }

    def test_accepts_valid_choices(self):
        assert problems_for([self._cmd([{"name": "One", "value": "one"}])]) == ""

    def test_rejects_more_than_25_choices(self):
        choices = [{"name": f"c{i}", "value": str(i)} for i in range(26)]
        msg = problems_for([self._cmd(choices)])
        assert "choices (maximum 25)" in msg

    def test_rejects_overlong_choice_name(self):
        msg = problems_for([self._cmd([{"name": "n" * 101, "value": "v"}])])
        assert "choice name" in msg and "exceeds 100 characters" in msg

    def test_rejects_overlong_choice_string_value(self):
        msg = problems_for([self._cmd([{"name": "n", "value": "v" * 101}])])
        assert "choice value" in msg and "exceeds 100 characters" in msg


# ---------------------------------------------------------------------------
# Relational choice/option checks (Fix 3): value type, required keys, non-empty,
# autocomplete/choices exclusivity, subcommand/leaf mixing, duplicate command
# names, non-string localization values.
# ---------------------------------------------------------------------------


class TestChoiceValueType:
    def _cmd(self, otype, value):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {
                    "type": otype,
                    "name": "o",
                    "description": "d",
                    "choices": [{"name": "n", "value": value}],
                }
            ],
        }

    def test_accepts_string_value_for_string_option(self):
        assert problems_for([self._cmd(3, "hello")]) == ""

    def test_accepts_integer_value_for_integer_option(self):
        assert problems_for([self._cmd(4, 5)]) == ""

    def test_accepts_number_value_for_number_option(self):
        assert problems_for([self._cmd(10, 1.5)]) == ""

    def test_rejects_integer_value_for_string_option(self):
        msg = problems_for([self._cmd(3, 5)])
        assert "must match the STRING option" in msg

    def test_rejects_string_value_for_integer_option(self):
        msg = problems_for([self._cmd(4, "5")])
        assert "must match the INTEGER option" in msg

    def test_rejects_string_value_for_number_option(self):
        msg = problems_for([self._cmd(10, "x")])
        assert "must match the NUMBER option" in msg


class TestChoiceRequiresNameAndValue:
    def _cmd(self, choice):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "o", "description": "d", "choices": [choice]}
            ],
        }

    def test_rejects_choice_missing_value(self):
        msg = problems_for([self._cmd({"name": "n"})])
        assert "must include a name and value" in msg

    def test_rejects_choice_missing_name(self):
        msg = problems_for([self._cmd({"value": "v"})])
        assert "must include a name and value" in msg


class TestChoicesNonEmpty:
    def test_rejects_empty_choices_array(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [{"type": 3, "name": "o", "description": "d", "choices": []}],
        }
        msg = problems_for([cmd])
        assert "must not be empty" in msg


class TestAutocompleteChoicesExclusive:
    def test_rejects_autocomplete_with_choices(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {
                    "type": 3,
                    "name": "o",
                    "description": "d",
                    "autocomplete": True,
                    "choices": [{"name": "n", "value": "v"}],
                }
            ],
        }
        msg = problems_for([cmd])
        assert "mutually exclusive" in msg

    def test_accepts_autocomplete_without_choices(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "o", "description": "d", "autocomplete": True}
            ],
        }
        assert problems_for([cmd]) == ""


class TestAutocompleteOptionType:
    # FIX 2: "autocomplete": true is only legal on STRING(3)/INTEGER(4)/NUMBER(10),
    # and must be rejected on other types even when no choices are present.
    def _cmd(self, otype):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": otype, "name": "o", "description": "d", "autocomplete": True}
            ],
        }

    def test_accepts_autocomplete_on_integer_option(self):
        assert problems_for([self._cmd(4)]) == ""

    def test_rejects_autocomplete_on_boolean_option(self):
        msg = problems_for([self._cmd(5)])
        assert "autocomplete is only valid on STRING, INTEGER, or NUMBER" in msg


class TestChoicesOnlyOnChoiceTypes:
    # FIX 3: static choices are only legal on STRING(3)/INTEGER(4)/NUMBER(10);
    # a BOOLEAN/USER/etc option carrying choices must be rejected.
    def _cmd(self, otype):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {
                    "type": otype,
                    "name": "o",
                    "description": "d",
                    "choices": [{"name": "n", "value": "v"}],
                }
            ],
        }

    def test_accepts_choices_on_string_option(self):
        assert problems_for([self._cmd(3)]) == ""

    def test_rejects_choices_on_user_option(self):
        msg = problems_for([self._cmd(6)])
        assert "choices are only valid on STRING, INTEGER, or NUMBER" in msg


class TestSubcommandGroupChildren:
    # FIX 4: a SUB_COMMAND_GROUP (type 2) may only contain SUB_COMMAND (type 1)
    # children; a leaf option directly inside a group must be rejected.
    def test_accepts_group_with_only_subcommands(self):
        cmd = {
            "name": "admin",
            "description": "d",
            "options": [
                {
                    "type": 2,
                    "name": "user",
                    "description": "d",
                    "options": [{"type": 1, "name": "add", "description": "d"}],
                }
            ],
        }
        assert problems_for([cmd]) == ""

    def test_rejects_leaf_option_inside_group(self):
        cmd = {
            "name": "admin",
            "description": "d",
            "options": [
                {
                    "type": 2,
                    "name": "user",
                    "description": "d",
                    "options": [{"type": 3, "name": "leaf", "description": "d"}],
                }
            ],
        }
        msg = problems_for([cmd])
        assert "may only contain subcommands" in msg


class TestChoiceNameBounds:
    # FIX 5: each choice name must be a string of 1..100 chars; empty and
    # non-string names must be rejected (not just overlong ones).
    def _cmd(self, name):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {
                    "type": 3,
                    "name": "o",
                    "description": "d",
                    "choices": [{"name": name, "value": "v"}],
                }
            ],
        }

    def test_accepts_valid_choice_name(self):
        assert problems_for([self._cmd("One")]) == ""

    def test_rejects_empty_choice_name(self):
        msg = problems_for([self._cmd("")])
        assert "choice name must not be empty" in msg

    def test_rejects_non_string_choice_name(self):
        msg = problems_for([self._cmd(123)])
        assert "choice name must be a string" in msg


class TestRequiredIsBoolean:
    # FIX 6: "required" must be a boolean when present; the string "false" (and
    # other non-booleans) must be rejected instead of silently coercing truthy.
    def _cmd(self, required):
        return {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "o", "description": "d", "required": required}
            ],
        }

    def test_accepts_boolean_required(self):
        assert problems_for([self._cmd(True)]) == ""

    def test_rejects_string_required(self):
        msg = problems_for([self._cmd("false")])
        assert "required must be a boolean" in msg


class TestSubcommandLeafMixing:
    def test_rejects_mixed_subcommand_and_leaf(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 1, "name": "sub", "description": "d"},
                {"type": 3, "name": "leaf", "description": "d"},
            ],
        }
        msg = problems_for([cmd])
        assert "must not mix subcommands" in msg

    def test_accepts_all_leaf_options(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "options": [
                {"type": 3, "name": "a", "description": "d"},
                {"type": 4, "name": "b", "description": "d"},
            ],
        }
        assert problems_for([cmd]) == ""


class TestDuplicateCommandNames:
    def test_rejects_duplicate_same_type(self):
        cmds = [
            {"name": "ping", "description": "d"},
            {"name": "ping", "description": "d"},
        ]
        msg = problems_for(cmds)
        assert "duplicate command name" in msg

    def test_accepts_same_name_different_type(self):
        # A chat-input "report" and a user context-menu "report" may coexist.
        cmds = [{"name": "report", "description": "d"}, {"name": "report", "type": 2}]
        assert problems_for(cmds) == ""


class TestLocalizationValueType:
    def test_rejects_non_string_localization_value(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "name_localizations": {"en-US": 123},
        }
        msg = problems_for([cmd])
        assert "must be a string" in msg


# ---------------------------------------------------------------------------
# Subcommand nesting depth <= 2 (group -> subcommand)
# ---------------------------------------------------------------------------


class TestNestingDepth:
    def test_accepts_group_then_subcommand(self):
        cmd = {
            "name": "admin",
            "description": "d",
            "options": [
                {
                    "type": 2,
                    "name": "user",
                    "description": "d",
                    "options": [
                        {
                            "type": 1,
                            "name": "ban",
                            "description": "d",
                            "options": [{"type": 6, "name": "target", "description": "d"}],
                        }
                    ],
                }
            ],
        }
        assert problems_for([cmd]) == ""

    def test_rejects_subcommand_inside_subcommand(self):
        cmd = {
            "name": "admin",
            "description": "d",
            "options": [
                {
                    "type": 1,
                    "name": "sub",
                    "description": "d",
                    "options": [{"type": 1, "name": "deeper", "description": "d"}],
                }
            ],
        }
        msg = problems_for([cmd])
        assert "nesting too deep" in msg


# ---------------------------------------------------------------------------
# default_member_permissions must be a string of digits
# ---------------------------------------------------------------------------


class TestDefaultMemberPermissions:
    def test_accepts_digit_string(self):
        assert (
            problems_for(
                [{"name": "cmd", "description": "d", "default_member_permissions": "8"}]
            )
            == ""
        )

    def test_rejects_non_digit_string(self):
        msg = problems_for(
            [{"name": "cmd", "description": "d", "default_member_permissions": "admin"}]
        )
        assert "default_member_permissions must be a string of digits" in msg

    def test_rejects_integer(self):
        msg = problems_for(
            [{"name": "cmd", "description": "d", "default_member_permissions": 8}]
        )
        assert "default_member_permissions must be a string of digits" in msg


# ---------------------------------------------------------------------------
# integration_types subset of {0,1}; contexts subset of {0,1,2}
# ---------------------------------------------------------------------------


class TestIntegrationTypesAndContexts:
    def test_accepts_valid_integration_types(self):
        assert (
            problems_for([{"name": "cmd", "description": "d", "integration_types": [0, 1]}])
            == ""
        )

    def test_rejects_invalid_integration_types(self):
        msg = problems_for([{"name": "cmd", "description": "d", "integration_types": [0, 2]}])
        assert "integration_types must be a subset" in msg

    def test_accepts_valid_contexts(self):
        assert (
            problems_for([{"name": "cmd", "description": "d", "contexts": [0, 1, 2]}]) == ""
        )

    def test_rejects_invalid_contexts(self):
        msg = problems_for([{"name": "cmd", "description": "d", "contexts": [0, 3]}])
        assert "contexts must be a subset" in msg


# ---------------------------------------------------------------------------
# Localizations: valid locales + value length rules
# ---------------------------------------------------------------------------


class TestLocalizations:
    def test_locale_list_is_module_constant(self):
        assert "en-US" in DISCORD_LOCALES and "zh-TW" in DISCORD_LOCALES

    def test_accepts_valid_name_localization(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "name_localizations": {"en-US": "cmd", "de": "befehl"},
            "description_localizations": {"en-US": "A command"},
        }
        assert problems_for([cmd]) == ""

    def test_rejects_unknown_locale(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "name_localizations": {"xx-YY": "cmd"},
        }
        msg = problems_for([cmd])
        assert "unknown locale" in msg

    def test_rejects_overlong_localized_name(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "name_localizations": {"en-US": "n" * 33},
        }
        msg = problems_for([cmd])
        assert "name_localizations" in msg and "1-32 characters" in msg

    def test_rejects_overlong_localized_description(self):
        cmd = {
            "name": "cmd",
            "description": "d",
            "description_localizations": {"en-US": "x" * 101},
        }
        msg = problems_for([cmd])
        assert "description_localizations" in msg and "1-100 characters" in msg


# ---------------------------------------------------------------------------
# Per-module command count reporting (info, not error)
# ---------------------------------------------------------------------------


class TestCommandCountReporting:
    def test_counts_commands_per_module(self, repo_root, make_module):
        install(
            make_module,
            [valid_chat_command(), {"name": "ping", "description": "d"}],
            name="demo",
        )
        make_module(
            "other",
            minimal_manifest("other"),
            commands=[{"name": "hi", "description": "d"}],
        )
        counts = command_count_by_module(repo_root)
        assert counts == {"demo": 2, "other": 1}

    def test_module_without_commands_file_counts_zero(self, repo_root, make_module):
        make_module("demo", minimal_manifest("demo"))  # no commands file on disk
        assert command_count_by_module(repo_root) == {"demo": 0}


class TestStrictEnumIntegers:
    def test_boolean_integration_type_rejected(self):
        import discord_modules

        problems = discord_modules.validate_command_schema(
            [{"name": "x", "description": "d", "integration_types": [True]}], "m"
        )
        assert any("integration_types" in p for p in problems)

    def test_float_context_rejected(self):
        import discord_modules

        problems = discord_modules.validate_command_schema(
            [{"name": "x", "description": "d", "contexts": [1.0]}], "m"
        )
        assert any("contexts" in p for p in problems)

    def test_integer_enums_accepted(self):
        import discord_modules

        problems = discord_modules.validate_command_schema(
            [
                {
                    "name": "x",
                    "description": "d",
                    "integration_types": [0, 1],
                    "contexts": [0, 1, 2],
                }
            ],
            "m",
        )
        assert not any("integration_types" in p or "contexts" in p for p in problems)
