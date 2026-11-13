#!/usr/bin/env python3
"""Regression checks for ownerApi source validation in the capability audit."""

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("audit_agent_capability_baseline.py")
SPEC = importlib.util.spec_from_file_location("agent_capability_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class OwnerApiValidationTests(unittest.TestCase):
    def validate(self, owner_api, source_texts):
        AUDIT.validate_owner_api_references(
            [{"operationId": "TEST_OPERATION", "ownerApi": owner_api}], source_texts)

    def assertRejected(self, owner_api, source_texts):
        with self.assertRaises(ValueError):
            self.validate(owner_api, source_texts)

    def test_method_must_belong_to_named_class(self):
        source = {"fixture.h": "class Owner { void valid(); }; class Other { void misplaced(); };"}
        self.validate("Owner::valid", source)
        self.assertRejected("Owner::misplaced", source)

    def test_every_compound_method_is_checked(self):
        source = {"fixture.h": "class Owner { void first(); void second(); };"}
        self.assertRejected("Owner::first/Owner::missing", source)

    def test_dead_constant_preprocessor_branches_are_ignored(self):
        source = {"fixture.h": """class Owner {
#if 0 && 1
void deadAnd();
#elif 0
void deadElif();
#else
void activeElse();
#endif
#if -1 / 2
void deadDivision();
#endif
#if ~0U < 0
void deadUnsigned();
#endif
#if 0xFFFFFFFFU + 1 != 0x100000000
void deadUnsignedWidth();
#endif
#if '\\0'
void deadCharacter();
#endif
#if '\\x00'
void deadHexCharacter();
#endif
#if '\\000'
void deadOctalCharacter();
#endif
#if 1 ? 0 : 1
void deadTernary();
#endif
#if 0 ? 1 : 1
void activeTernary();
#endif
#if !1 == 2
void deadNotPrecedence();
#endif
#if !0 == 1
void activeNotPrecedence();
#endif
#if !-!0
void deadChainedUnary();
#endif
};
"""}
        self.assertRejected("Owner::deadAnd", source)
        self.assertRejected("Owner::deadElif", source)
        self.assertRejected("Owner::deadDivision", source)
        self.assertRejected("Owner::deadUnsigned", source)
        self.assertRejected("Owner::deadUnsignedWidth", source)
        self.assertRejected("Owner::deadCharacter", source)
        self.assertRejected("Owner::deadHexCharacter", source)
        self.assertRejected("Owner::deadOctalCharacter", source)
        self.assertRejected("Owner::deadTernary", source)
        self.assertRejected("Owner::deadNotPrecedence", source)
        self.assertRejected("Owner::deadChainedUnary", source)
        self.validate("Owner::activeElse", source)
        self.validate("Owner::activeTernary", source)
        self.validate("Owner::activeNotPrecedence", source)

    def test_unknown_macro_branches_remain_possible(self):
        source = {"fixture.h": "class Owner {\n#if FEATURE_FLAG\nvoid maybePresent();\n#endif\n};"}
        self.validate("Owner::maybePresent", source)

    def test_comments_and_raw_strings_cannot_supply_object_names(self):
        source = {"fixture.cpp": """// button->setObjectName(\"CommentOnlyBtn\");
auto raw = R\"tag(button->setObjectName(\"RawStringOnlyBtn\"))tag\";
button->setObjectName(QStringLiteral(\"RealButton\"));
"""}
        self.assertRejected("CommentOnlyBtn", source)
        self.assertRejected("RawStringOnlyBtn", source)
        self.validate("RealButton", source)

    def test_duplicate_class_names_are_ambiguous(self):
        source = {"fixture.h": "namespace A { class Owner { void action(); }; }\n"
                               "namespace B { class Owner { void action(); }; }"}
        self.assertRejected("Owner::action", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
