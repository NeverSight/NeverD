from __future__ import annotations

import unittest

from scripts import check_agent_qml_text as text


class CheckAgentQmlTextTest(unittest.TestCase):
    def problems(self, source: str, name: str = "Probe.qml") -> list[str]:
        return text.check_text(name, source)[0]

    def test_repository_window_shows_text_as_text(self) -> None:
        files = sorted(text.UI.glob("*.qml"))
        self.assertTrue(files)
        for path in files:
            self.assertEqual(text.check_text(path.name, path.read_text(encoding="utf-8"))[0], [])

    def test_label_without_a_format_is_reported_with_its_line(self) -> None:
        found = self.problems("Item {\n    Label { text: backend.name }\n}\n")
        self.assertEqual(found, ["Probe.qml:2: Label needs textFormat: Text.PlainText"])

    def test_plain_labels_pass_on_one_line_and_on_several(self) -> None:
        self.assertEqual(self.problems(
            "Label { textFormat: Text.PlainText; text: backend.name }\n"
            "Text {\n    textFormat: Text.PlainText\n    text: backend.path\n}\n"), [])

    def test_a_child_does_not_state_its_parent_format(self) -> None:
        found = self.problems(
            "Label {\n    text: backend.name\n"
            "    Text { textFormat: Text.PlainText; text: \"x\" }\n}\n")
        self.assertEqual(found, ["Probe.qml:1: Label needs textFormat: Text.PlainText"])

    def test_braces_in_strings_and_comments_do_not_end_an_element(self) -> None:
        self.assertEqual(self.problems(
            "Label {\n    text: \"}\" // }\n    /* } */\n"
            "    textFormat: Text.PlainText\n}\n"), [])

    def test_a_format_in_a_comment_does_not_count(self) -> None:
        found = self.problems("Label {\n    // textFormat: Text.PlainText\n    text: name\n}\n")
        self.assertEqual(found, ["Probe.qml:1: Label needs textFormat: Text.PlainText"])

    def test_enum_uses_and_other_types_are_not_elements(self) -> None:
        self.assertEqual(self.problems(
            "ThemedToolTip { text: hint }\nItem { property int mode: Text.ElideRight }\n"), [])

    def test_markup_formats_are_reported(self) -> None:
        found = self.problems("Text { textFormat: Text.StyledText; text: name }\n")
        self.assertEqual(len(found), 2)
        self.assertIn("Text.StyledText lays out markup", found[0])

    def test_rich_text_is_kept_to_the_guarded_view(self) -> None:
        source = "TextEdit { textFormat: TextEdit.RichText }\n"
        self.assertEqual(self.problems(source, "MarkdownView.qml"), [])
        self.assertEqual(len(self.problems(source, "MemoryView.qml")), 1)

    def test_an_image_takes_a_literal_resource(self) -> None:
        self.assertEqual(self.problems('Image { source: "qrc:/agent/icons/brand.svg" }\n'), [])
        self.assertEqual(len(self.problems("Image { source: modelData.icon }\n")), 1)
        self.assertEqual(len(self.problems('Image { source: "https://example.org/a.png" }\n')), 1)
        self.assertEqual(len(self.problems("Image { width: 20 }\n")), 1)

    def test_a_control_shows_only_written_words_in_its_own_label(self) -> None:
        self.assertEqual(self.problems(
            'ToolButton { text: qsTr("Copy") }\n'
            'ToolButton { text: copied ? qsTr("Copied") : qsTr("Copy") }\n'
            'CheckBox { text: "Plain" }\n'
            'ToolButton { glyph: "more" }\n'), [])
        for source in ("ToolButton { text: modelData.name }\n",
                       "MenuItem { text: chat.title }\n",
                       'TabButton { text: qsTr("%1 · Evidence").arg(view.title) }\n',
                       'ToolButton { text: busy ? record.name : qsTr("Idle") }\n',
                       "delegate: ItemDelegate { text: modelData }\n"):
            found = self.problems(source)
            self.assertEqual(len(found), 1, source)
            self.assertIn("default format", found[0])

    def test_a_control_that_lays_out_its_text_may_show_data(self) -> None:
        self.assertEqual(self.problems(
            "TabButton {\n    text: view.title\n"
            "    contentItem: Text { textFormat: Text.PlainText; text: parent.text }\n}\n"), [])
        found = self.problems(
            "TabButton {\n    text: view.title\n    contentItem: Text { text: parent.text }\n}\n")
        self.assertEqual(found, ["Probe.qml:3: Text needs textFormat: Text.PlainText"])

    def test_a_dialog_title_is_written_or_given_a_header(self) -> None:
        self.assertEqual(self.problems('Dialog { title: qsTr("Settings") }\n'), [])
        self.assertEqual(self.problems(
            "Dialog {\n    title: project.name\n"
            "    header: Label { textFormat: Text.PlainText; text: parent.title }\n}\n"), [])
        self.assertEqual(len(self.problems("Dialog { title: project.name }\n")), 1)

    def test_a_text_input_names_its_context_menu(self) -> None:
        self.assertEqual(self.problems(
            "TextField { id: field; ContextMenu.menu: EditMenu { editor: field } }\n"
            "TextArea {\n    id: area\n    ContextMenu.menu: EditMenu { editor: area }\n}\n"
            "Field { placeholderText: qsTr(\"Name\") }\nChatComposer { id: composer }\n"), [])
        for source in ("TextField { text: name }\n",
                       "TextArea {\n    readOnly: true\n}\n",
                       "ScrollView { TextArea { id: a }\n    ContextMenu.menu: EditMenu {} }\n"):
            found = self.problems(source)
            self.assertEqual(len(found), 1, source)
            self.assertIn("context menu", found[0])

    def test_written_tells_words_in_the_source_from_data(self) -> None:
        for expression in ('"x"', 'qsTr("x")', 'a ? "x" : qsTr("y")',
                           'a ? "x" : b ? qsTr("y") : "z"', 'f(a ? 1 : 2) ? "x" : "y"',
                           'qsTr("a ? b : c")', r'qsTr("say \"hi\"")'):
            self.assertTrue(text.written(expression), expression)
        for expression in ("name", 'qsTr("%1").arg(name)', 'a ? name : "x"', '"x" + name',
                           'qsTr("x"', '"x")', ""):
            self.assertFalse(text.written(expression), expression)


if __name__ == "__main__":
    unittest.main()
