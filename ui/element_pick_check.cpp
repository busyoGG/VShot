// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the picker's UI-element level: the wheel gestures that
// move between a window, its widgets, and the widgets inside those.
//
// The behaviour worth pinning is the shape of the two gestures, not the
// picking of an element by position alone:
//
//   * wheel up climbs to the parent, and keeps climbing until the whole window
//     is selected -- the level picking started at, so a user can always get
//     back to capturing the window;
//   * wheel down retraces the way it came, so it returns to the element the
//     user left rather than to some other child of the parent;
//   * a node no one can point at -- zero-sized, as a tenth of a real tree is --
//     is dropped rather than offered.
//
// Needs no compositor and no overlay: `ElementTree` is the whole second level
// and is driven directly.  Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the
// README's verification section.

#include "session_protocol.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what, const QString &detail = QString())
{
    if (condition) {
        std::printf("ok    %s\n", what);
        return;
    }
    ++failures;
    if (detail.isEmpty()) {
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("FAIL  %s -- %s\n", what, qPrintable(detail));
    }
}

QJsonObject node(const char *label, int x, int y, int width, int height, int parent)
{
    QJsonObject object;
    object.insert(QStringLiteral("label"), QString::fromUtf8(label));
    object.insert(QStringLiteral("x"), x);
    object.insert(QStringLiteral("y"), y);
    object.insert(QStringLiteral("width"), width);
    object.insert(QStringLiteral("height"), height);
    if (parent >= 0) {
        object.insert(QStringLiteral("parent"), parent);
    }
    return object;
}

//  window
//    panel          0..100 x 0..100
//      button        10..50 x 10..50    (node 1)
//        label       20..40 x 20..40    (node 2)
//      slider        60..90 x 10..50    (node 3)
//    statusbar       0..100 x 90..100   (node 4)
QJsonArray twoLevels()
{
    QJsonArray array;
    array.push_back(node("panel", 0, 0, 100, 100, -1));
    array.push_back(node("button", 10, 10, 40, 40, 0));
    array.push_back(node("label", 20, 20, 20, 20, 1));
    array.push_back(node("slider", 60, 10, 30, 40, 0));
    array.push_back(node("statusbar", 0, 90, 100, 10, -1));
    return array;
}

const char *labelOf(const vshot::ElementTree &tree, int index)
{
    const vshot::ElementTree::Node *found = tree.node(index);
    return found == nullptr ? "<window>" : qPrintable(found->label);
}

} // namespace

int main()
{
    using vshot::ElementTree;

    // --- Picking by position ---------------------------------------------
    {
        ElementTree tree;
        tree.load(twoLevels());

        expect(tree.size() == 5, "every node with an extent is kept");
        expect(tree.indexAt(25, 25) == 2, "a point inside the label names the label",
               QStringLiteral("got %1").arg(tree.indexAt(25, 25)));
        expect(tree.indexAt(15, 15) == 1, "a point in the button but not the label stops there");
        expect(tree.indexAt(70, 30) == 3, "a point in the slider names the slider");
        expect(tree.indexAt(50, 95) == 4, "a point in the statusbar names the statusbar");
        expect(tree.indexAt(500, 500) == -1, "a point in no element is the window");
    }

    // A node drawn over a sibling is the one picked, not the smaller one --
    // which is the rule the window level uses too.
    {
        QJsonArray array;
        array.push_back(node("under", 0, 0, 100, 100, -1));
        array.push_back(node("over", 0, 0, 100, 100, -1));
        ElementTree tree;
        tree.load(array);
        expect(tree.indexAt(50, 50) == 1, "the sibling written later is the one on top");
    }

    // --- Climbing out ------------------------------------------------------
    {
        ElementTree tree;
        tree.load(twoLevels());
        tree.select(tree.indexAt(25, 25));
        expect(tree.current() == 2, "the highlight starts on the deepest node");

        expect(tree.step(true), "one step up is possible");
        expect(tree.current() == 1, "wheeling up once reaches the button", labelOf(tree, tree.current()));
        expect(tree.step(true), "a second step up is possible");
        expect(tree.current() == 0, "wheeling up twice reaches the panel", labelOf(tree, tree.current()));
        expect(tree.step(true), "a third step up is possible");
        expect(tree.current() == -1, "wheeling up off the top reaches the window");
        // The window is the ceiling: further steps up change nothing.
        expect(!tree.step(true), "there is nothing above the window");
        expect(tree.current() == -1, "the highlight stays on the window");
    }

    // --- Retracing the way down -------------------------------------------
    {
        ElementTree tree;
        tree.load(twoLevels());
        tree.select(tree.indexAt(25, 25));
        // To the window, then back down the same nodes.
        tree.step(true);
        tree.step(true);
        tree.step(true);
        expect(tree.current() == -1, "climbed to the window");

        expect(tree.step(false), "a step back down is possible");
        expect(tree.current() == 0, "down returns to the panel, not some other child",
               labelOf(tree, tree.current()));
        expect(tree.step(false), "a second step down is possible");
        expect(tree.current() == 1, "down returns to the button", labelOf(tree, tree.current()));
        expect(tree.step(false), "a third step down is possible");
        expect(tree.current() == 2, "down returns to the label it left", labelOf(tree, tree.current()));
        expect(!tree.step(false), "the descent is spent");
        expect(tree.current() == 2, "the highlight stays where the descent ended");
    }

    // A descent taken one step and then reversed: the path is what the user
    // travelled, not the whole depth of the tree.
    {
        ElementTree tree;
        tree.load(twoLevels());
        tree.select(1);
        tree.step(true); // to the panel
        expect(tree.current() == 0, "one step up from the button is the panel");
        tree.step(false);
        expect(tree.current() == 1, "back down returns to the button, not the label",
               labelOf(tree, tree.current()));
        expect(!tree.step(false), "and no further");
    }

    // --- Nodes nobody can point at ----------------------------------------
    {
        QJsonArray array;
        array.push_back(node("zero", 0, 0, 0, 10, -1));
        array.push_back(node("negative", 0, 0, 10, 0, -1));
        array.push_back(node("real", 0, 0, 10, 10, -1));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 1, "a node with no extent is dropped",
               QStringLiteral("kept %1").arg(tree.size()));
        expect(tree.indexAt(5, 5) == 0, "what is left is reachable");
    }

    // A window with no tree: nothing to step through, and stepping says so.
    {
        ElementTree tree;
        expect(tree.isEmpty(), "a fresh tree is empty");
        expect(!tree.step(true), "no step up on an empty tree");
        expect(!tree.step(false), "no step down on an empty tree");
        expect(tree.current() == -1, "an empty tree is on the window");
        tree.load(QJsonArray());
        expect(tree.isEmpty(), "an answer with no elements leaves it empty");
    }

    // Clearing drops the tree and the level with it.
    {
        ElementTree tree;
        tree.load(twoLevels());
        tree.select(2);
        tree.clear();
        expect(tree.isEmpty(), "clearing drops the nodes");
        expect(tree.current() == -1, "clearing returns the highlight to the window");
    }

    // --- Wrappers are collapsed ------------------------------------------
    //
    // A toolkit nests plain containers for layout, and a container whose only
    // visible child covers exactly the same rectangle is not a level the user
    // can tell apart from the one inside it: stopping there makes the wheel
    // look broken.  The two are one node.
    {
        QJsonArray array;
        array.push_back(node("filler", 0, 0, 100, 50, -1));
        array.push_back(node("real", 0, 0, 100, 50, 0));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 1, "a same-sized only child is collapsed into its wrapper",
               QStringLiteral("kept %1").arg(tree.size()));
        expect(tree.node(0) != nullptr && tree.node(0)->label == QStringLiteral("real"),
               "the named child's label survives the collapse");
        expect(tree.indexAt(50, 25) == 0, "the collapsed node is still reachable");
    }

    // A wrapper chain -- several in a row -- collapses to one node.
    {
        QJsonArray array;
        array.push_back(node("a", 0, 0, 100, 50, -1));
        array.push_back(node("b", 0, 0, 100, 50, 0));
        array.push_back(node("c", 0, 0, 100, 50, 1));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 1, "a chain of wrappers collapses to one node",
               QStringLiteral("kept %1").arg(tree.size()));
    }

    // Only a *single* child collapses: two children is a real branching level
    // even when both happen to cover the parent.
    {
        QJsonArray array;
        array.push_back(node("parent", 0, 0, 100, 50, -1));
        array.push_back(node("left", 0, 0, 100, 50, 0));
        array.push_back(node("right", 0, 0, 100, 50, 0));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 3, "two children are still two levels",
               QStringLiteral("kept %1").arg(tree.size()));
    }

    // A child that only partly covers its parent is not a wrapper: the parent
    // is a real region of its own.
    {
        QJsonArray array;
        array.push_back(node("outer", 0, 0, 100, 50, -1));
        array.push_back(node("inner", 10, 10, 40, 20, 0));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 2, "a child that does not fill its parent is not collapsed",
               QStringLiteral("kept %1").arg(tree.size()));
        expect(tree.node(1)->parent == 0, "and stays a child of it");
    }

    // Collapsing keeps the rest of the tree's shape: a sibling of the wrapper
    // still hangs off the wrapper's parent.
    {
        QJsonArray array;
        array.push_back(node("root", 0, 0, 100, 100, -1));
        array.push_back(node("wrapper", 0, 0, 50, 100, 0));
        array.push_back(node("inside", 0, 0, 50, 100, 1));
        array.push_back(node("sibling", 50, 0, 50, 100, 0));
        ElementTree tree;
        tree.load(array);
        expect(tree.size() == 3, "the wrapper collapses and the sibling stays",
               QStringLiteral("kept %1").arg(tree.size()));
        expect(tree.node(1)->label == QStringLiteral("inside"), "the wrapper's child takes its place");
        expect(tree.node(1)->parent == 0, "and hangs off the wrapper's parent");
        expect(tree.node(2)->label == QStringLiteral("sibling"), "the sibling is untouched");
        expect(tree.node(2)->parent == 0, "and still hangs off the root");
    }

    // --- A real Chromium tree --------------------------------------------
    //
    // Captured from a live `vshot window pick` against a Chromium window
    // launched with --force-renderer-accessibility: 76 nodes.  It is here
    // because its top is three nested `panel`s all covering the window
    // exactly -- the wrapper shape toolkits produce, and the reason a wheel
    // appeared to do nothing: without collapsing, the first three levels are
    // the same full-window box.
    {
        QJsonArray array;
        array.push_back(node("panel", 5, 38, 2550, 1397, -1));
        array.push_back(node("panel", 5, 38, 2550, 1397, 0));
        array.push_back(node("panel", 5, 38, 2550, 1397, 1));
        array.push_back(node("panel", 245, 84, 2310, 1351, 2));
        array.push_back(node("panel", 245, 38, 2310, 46, 2));
        array.push_back(node("tool bar", 279, 38, 2276, 46, 4));
        array.push_back(node("button 返回", 279, 44, 40, 34, 5));
        array.push_back(node("button 前进", 321, 44, 34, 34, 5));
        array.push_back(node("button 重新加载", 357, 44, 34, 34, 5));
        array.push_back(node("panel", 400, 44, 1746, 34, 5));
        array.push_back(node("button 查看网站信息", 405, 49, 24, 24, 9));
        array.push_back(node("entry 地址和搜索栏", 437, 49, 1633, 24, 9));
        array.push_back(node("panel", 2078, 49, 56, 24, 9));
        array.push_back(node("button 安装“DeepSeek”", 2078, 49, 24, 24, 12));
        array.push_back(node("button 为此标签页修改书签", 2110, 49, 24, 24, 12));
        array.push_back(node("panel", 2155, 44, 250, 34, 5));
        array.push_back(node("button Tampermonkey Editors", 2227, 44, 34, 34, 15));
        array.push_back(node("button Shazam：在浏览器中直接识别歌曲", 2299, 44, 34, 34, 15));
        array.push_back(node("button 扩展程序", 2371, 44, 34, 34, 15));
        array.push_back(node("panel", 2414, 53, 2, 16, 5));
        array.push_back(node("panel", 2425, 44, 52, 34, 5));
        array.push_back(node("toggle button 下载内容 - 已固定", 2425, 44, 34, 34, 20));
        array.push_back(node("panel", 2468, 53, 2, 16, 20));
        array.push_back(node("button 工作", 2479, 44, 34, 34, 5));
        array.push_back(node("button Chromium", 2515, 44, 40, 34, 5));
        array.push_back(node("panel", 245, 84, 2310, 1351, 2));
        array.push_back(node("panel", 245, 84, 2310, 1351, 25));
        array.push_back(node("panel", 245, 85, 2310, 1350, 25));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 28));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("document web DeepSeek - 探索未至之境", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 2387, 1411, 168, 24, 27));
        array.push_back(node("static chat.deepseek.com", 2409, 1411, 116, 23, 37));
        array.push_back(node("button 关闭此视图", 2531, 1411, 24, 24, 37));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 85, 2310, 1350, 27));
        array.push_back(node("panel", 245, 84, 2310, 1, 25));
        array.push_back(node("panel", 245, 84, 2310, 1351, 25));
        array.push_back(node("panel", 245, 84, 2310, 1351, 2));
        array.push_back(node("panel", 244, 83, 10, 10, 44));
        array.push_back(node("panel", 2546, 83, 9, 10, 44));
        array.push_back(node("panel", 244, 1426, 10, 9, 44));
        array.push_back(node("panel", 2546, 1426, 9, 9, 44));
        array.push_back(node("panel", 245, 84, 2310, 1351, 44));
        array.push_back(node("page tab list", 5, 38, 240, 1397, 2));
        array.push_back(node("panel", 17, 38, 216, 46, 50));
        array.push_back(node("button 收起标签页", 17, 45, 32, 32, 51));
        array.push_back(node("panel", 175, 47, 58, 28, 51));
        array.push_back(node("button 标签页分组", 175, 47, 28, 28, 53));
        array.push_back(node("button 标签页搜索", 205, 47, 28, 28, 53));
        array.push_back(node("panel", 17, 84, 216, 1, 50));
        array.push_back(node("panel", 5, 93, 240, 70, 50));
        array.push_back(node("panel", 17, 93, 228, 32, 57));
        array.push_back(node("panel", 17, 93, 228, 32, 58));
        array.push_back(node("panel", 17, 93, 228, 32, 59));
        array.push_back(node("panel", 5, 133, 240, 30, 57));
        array.push_back(node("panel", 5, 133, 240, 30, 61));
        array.push_back(node("panel", 5, 133, 240, 30, 62));
        array.push_back(node("panel", 17, 171, 216, 1252, 50));
        array.push_back(node("button 打开新的标签页", 17, 171, 216, 32, 64));
        array.push_back(node("slider 垂直标签栏大小调整手柄（可拖动）", 240, 38, 5, 1397, 50));
        array.push_back(node("panel", 5, 38, 240, 1397, 50));
        array.push_back(node("panel", 244, 38, 10, 8, 2));
        array.push_back(node("panel", 244, 1427, 10, 8, 2));
        array.push_back(node("panel", 5, 38, 2550, 1397, 2));
        array.push_back(node("panel", 5, 38, 2550, 1397, 0));
        ElementTree tree;
        tree.load(array);

        // The three same-sized wrappers at the top are one level, not three.
        expect(tree.size() > 0, "the real tree loads at all");
        expect(tree.size() < 76, "wrappers in the real tree are collapsed",
               QStringLiteral("kept %1 of 76").arg(tree.size()));
        const vshot::ElementTree::Node *top = tree.node(0);
        expect(top != nullptr && top->rect.width == 2550 && top->rect.height == 1397,
               "the top of the collapsed tree is still the window-sized panel");
        // And the tree still has real depth below it: a browser's toolbar and
        // page are levels a user can actually choose.
        int depth = 0;
        for (int i = 0; i < tree.size(); ++i) {
            int d = 0;
            for (int p = tree.node(i)->parent; p >= 0; p = tree.node(p)->parent) {
                ++d;
                if (d > 32) {
                    break;
                }
            }
            depth = std::max(depth, d);
        }
        expect(depth >= 4, "the collapsed tree still has levels to step through",
               QStringLiteral("deepest %1").arg(depth));
        // A point in the toolbar area lands on something smaller than the
        // window, which is the whole point of the element level.
        const int hit = tree.indexAt(300, 55);
        const vshot::ElementTree::Node *found = tree.node(hit);
        expect(hit >= 0 && found != nullptr && found->rect.width < 2550,
               "a point in the toolbar picks a toolbar-sized element",
               found == nullptr ? QStringLiteral("no hit")
                                : QStringLiteral("%1x%2").arg(found->rect.width).arg(found->rect.height));
    }

    std::printf("\n%s\n", failures == 0 ? "all element-pick checks passed"
                                        : "ELEMENT-PICK CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
