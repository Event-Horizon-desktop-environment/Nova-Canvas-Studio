#include "UX/theme_state.hpp"
#include "UX/theme_tokens.hpp"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdio>

using namespace canvas::gui;

namespace {

bool expect(bool cond, const char* label, bool& ok) {
    if (!cond) {
        std::printf("FAIL: %s\n", label);
        ok = false;
        return false;
    }
    return true;
}

}

static int run_checks() {
    bool ok = true;
    unsigned failures = 0;

    {
        QSet<QString> seen;
        for (int i = 0; i < kThemeTokenFieldCount; ++i) {
            const auto f = static_cast<ThemeTokenField>(i);
            const QString name = QString::fromUtf8(theme_token_field_name(f));
            expect(!name.isEmpty(), "field name non-empty", ok);
            expect(!seen.contains(name), "field names unique", ok);
            expect(theme_setting_key(f).startsWith(QStringLiteral("settings/theme/")),
                   "field key prefixed by settings/theme/", ok);
            seen.insert(name);
        }
        expect(seen.size() == kThemeTokenFieldCount, "enum fully enumerated", ok);
        expect(theme_setting_key(ThemeTokenField::Accent)
                   == QStringLiteral("settings/theme/accent"),
               "accent key stable", ok);
        expect(theme_setting_key(ThemeTokenField::SurfaceLow)
                   == QStringLiteral("settings/theme/surface_low"),
               "surface_low key stable", ok);
        expect(QString::fromUtf8(theme_token_field_name(ThemeTokenField::COUNT_)).isEmpty(),
               "COUNT_ has no name", ok);
    }

    {
        expect(theme_color_string(QColor(0xEA, 0xA3, 0x40))
                   == QStringLiteral("#eaa340"),
               "opaque color serialized as HexRgb", ok);
        const QColor alpha_col(0x55, 0x00, 0x7F, 0x24);
        const QString s = theme_color_string(alpha_col);
        expect(s == alpha_col.name(QColor::HexArgb), "translucent keeps HexArgb", ok);
        expect(theme_color_string(QColor()).isEmpty(), "invalid serializes empty", ok);
        expect(QColor(s) == alpha_col, "HexArgb string parses back", ok);
    }

    {
        const QColor designed = designed_token_value(ThemeTokenField::Surface);
        const QColor over(0x10, 0x20, 0x30);
        set_token_override(ThemeTokenField::Surface, over);
        expect(token_override(ThemeTokenField::Surface) == over,
               "override visible to token_override", ok);
        expect(tokens().surface == over, "override applied to tokens()", ok);
        set_token_override(ThemeTokenField::Surface, QColor());
        expect(!token_override(ThemeTokenField::Surface).isValid(),
               "invalid color clears override", ok);
        expect(tokens().surface == designed, "tokens() returns designed after clear", ok);
    }

    {
        const ThemeTokens base = tokens();
        const QColor purple(0x55, 0x00, 0x7F);
        set_token_override(ThemeTokenField::Accent, purple);
        expect(tokens().accent == purple, "accent override lands", ok);
        expect(tokens().accent_hover != base.accent_hover,
               "accent_hover recolored", ok);
        expect(tokens().accent_press != base.accent_press,
               "accent_press recolored", ok);
        expect(tokens().on_accent != base.on_accent,
               "on_accent recolored", ok);
        expect(tokens().accent_text == base.accent_text,
               "accent_text keeps designed color", ok);
        expect(tokens().accent_soft == base.accent_soft,
               "accent_soft keeps designed color+alpha", ok);
        expect(tokens().accent_line == base.accent_line,
               "accent_line keeps designed color", ok);
        expect(tokens().state_selected == base.state_selected,
               "state_selected keeps designed color", ok);
        expect(tokens().focus_ring == base.focus_ring,
               "focus_ring keeps designed color", ok);
        set_token_override(ThemeTokenField::Accent, QColor(0x55, 0x00, 0x7F, 0x99));
        expect(tokens().accent == QColor(0x55, 0x00, 0x7F, 0x99),
               "translucent accent override lands verbatim", ok);
        set_token_override(ThemeTokenField::Accent, QColor());
        expect(tokens().accent == base.accent && tokens().accent_hover == base.accent_hover,
               "accent family returns to designed on clear", ok);
    }

    {
        const ThemeTokens base = tokens();
        set_token_override(ThemeTokenField::Playhead, QColor(0x4F, 0xB8, 0xD6));
        expect(tokens().playhead == QColor(0x4F, 0xB8, 0xD6), "playhead override lands", ok);
        expect(tokens().playhead_soft.alpha() == base.playhead_soft.alpha(),
               "playhead_soft keeps designed alpha", ok);
        set_token_override(ThemeTokenField::Playhead, QColor());

        set_token_override(ThemeTokenField::Ink, QColor(0x20, 0x20, 0x20));
        expect(tokens().state_hover.alpha() == base.state_hover.alpha(),
               "state_hover keeps designed alpha", ok);
        expect(tokens().ink == QColor(0x20, 0x20, 0x20), "ink override lands", ok);
        set_token_override(ThemeTokenField::Ink, QColor());
    }

    {
        const ThemeTokens base = tokens();
        const QColor b(0x30, 0x30, 0x35);
        set_token_override(ThemeTokenField::Border, b);
        expect(tokens().border == b, "border override lands", ok);
        expect(tokens().border_hi == b, "border_hi follows border", ok);
        set_token_override(ThemeTokenField::Border, QColor());
    }

    {
        const QColor designed_accent_soft = tokens().accent_soft;
        const QColor accent(0x22, 0x88, 0x44);
        const QColor playhead(0x4F, 0xB8, 0xD6, 0xCC);
        const QColor border(0x99, 0x88, 0x77, 0x33);
        set_token_override(ThemeTokenField::Accent, accent);
        set_token_override(ThemeTokenField::Playhead, playhead);
        set_token_override(ThemeTokenField::Border, border);

        QTemporaryDir dir;
        expect(dir.isValid(), "temp dir for theme export", ok);
        const QString path = dir.filePath(QStringLiteral("share.canvas-theme.json"));
        expect(export_theme_file(path, QStringLiteral("Test Theme")),
               "export_theme_file succeeds", ok);

        QFile f(path);
        expect(f.open(QIODevice::ReadOnly), "exported file readable", ok);
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        const QJsonObject root = doc.object();
        expect(root.value(QStringLiteral("format")).toString()
                   == QStringLiteral("nova-canvas-theme"),
               "file advertises nova-canvas-theme", ok);
        expect(root.value(QStringLiteral("name")).toString()
                   == QStringLiteral("Test Theme"),
               "file stores the theme name", ok);
        const QJsonObject overrides = root.value(QStringLiteral("overrides")).toObject();
        expect(overrides.contains(QStringLiteral("accent")), "accent exported", ok);
        expect(overrides.contains(QStringLiteral("playhead")), "playhead exported", ok);
        expect(overrides.contains(QStringLiteral("border")), "border exported", ok);
        expect(!overrides.contains(QStringLiteral("surface")),
               "unset fields omitted from export", ok);
        expect(QColor(overrides.value(QStringLiteral("accent")).toString()) == accent,
               "accent hex round-trips", ok);
        expect(QColor(overrides.value(QStringLiteral("playhead")).toString()) == playhead,
               "translucent playhead hex round-trips", ok);

        for (int i = 0; i < kThemeTokenFieldCount; ++i)
            set_token_override(static_cast<ThemeTokenField>(i), QColor());
        expect(!token_override(ThemeTokenField::Accent).isValid(), "overrides wiped", ok);

        QString imported_name;
        expect(import_theme_file(path, &imported_name), "import_theme_file succeeds", ok);
        expect(imported_name == QStringLiteral("Test Theme"), "imported name reported", ok);
        expect(token_override(ThemeTokenField::Accent) == accent,
               "import restores accent", ok);
        expect(token_override(ThemeTokenField::Playhead) == playhead,
               "import restores translucent playhead", ok);
        expect(token_override(ThemeTokenField::Border) == border,
               "import restores translucent border", ok);
        expect(!token_override(ThemeTokenField::Surface).isValid(),
               "import leaves unset fields untouched", ok);
        expect(tokens().accent == accent, "imported override live in tokens()", ok);
        expect(tokens().accent_soft == designed_accent_soft,
               "imported accent leaves the chrome's soft fills untouched", ok);

        for (int i = 0; i < kThemeTokenFieldCount; ++i)
            set_token_override(static_cast<ThemeTokenField>(i), QColor());
    }

    {
        const QColor before = token_override(ThemeTokenField::Accent);
        QTemporaryDir dir;
        const QString junk = dir.filePath(QStringLiteral("junk.json"));
        QFile j(junk);
        expect(j.open(QIODevice::WriteOnly), "junk file writable", ok);
        j.write("this is not a theme file");
        j.close();
        expect(!import_theme_file(junk, nullptr), "non-JSON rejected", ok);

        const QString foreign = dir.filePath(QStringLiteral("foreign.json"));
        QFile x(foreign);
        expect(x.open(QIODevice::WriteOnly), "foreign file writable", ok);
        x.write("{\"format\":\"some-other-app\",\"overrides\":{}}");
        x.close();
        expect(!import_theme_file(foreign, nullptr), "foreign format rejected", ok);

        const QString derived = dir.filePath(QStringLiteral("derived.json"));
        QFile d(derived);
        expect(d.open(QIODevice::WriteOnly), "derived file writable", ok);
        d.write("{\"format\":\"nova-canvas-theme\",\"version\":1,"
                "\"overrides\":{\"accent_hover\":\"#ff0000\"}}");
        d.close();
        expect(import_theme_file(derived, nullptr), "derived-only file accepted", ok);
        expect(token_override(ThemeTokenField::Accent) == before,
               "unknown fields skipped on import", ok);
    }

    failures = ok ? 0 : 1;
    std::printf(ok ? "ALL THEME ROUNDTRIP TESTS PASSED\n"
                   : "THEME ROUNDTRIP TEST(S) FAILED\n");
    return failures;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    return run_checks();
}
