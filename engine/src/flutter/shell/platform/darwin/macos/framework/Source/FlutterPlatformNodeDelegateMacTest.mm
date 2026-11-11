// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/testing/testing.h"

#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/AccessibilityBridgeMac.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterDartProject_Internal.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine_Internal.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterPlatformNodeDelegateMac.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterTextInputPlugin.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterTextInputSemanticsObject.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterViewControllerTestUtils.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterViewController_Internal.h"

#include "flutter/shell/platform/common/accessibility_bridge.h"
#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/third_party/accessibility/ax/ax_action_data.h"

namespace flutter::testing {

namespace {
// Returns a view controller configured for the text fixture resource configuration.
FlutterViewController* CreateTestViewController() {
  NSString* fixtures = @(testing::GetFixturesPath());
  FlutterDartProject* project = [[FlutterDartProject alloc]
      initWithAssetsPath:fixtures
             ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];
  return [[FlutterViewController alloc] initWithProject:project];
}
}  // namespace

TEST(FlutterPlatformNodeDelegateMac, Basics) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  engine.semanticsEnabled = YES;
  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{0};
  root.id = 0;
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.text_selection_base = -1;
  root.text_selection_extent = -1;
  root.label = "accessibility";
  root.hint = "";
  root.value = "";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 0;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  bridge->AddFlutterSemanticsNodeUpdate(root);

  bridge->CommitUpdates();

  auto root_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(0).lock();
  // Verify the accessibility attribute matches.
  NSAccessibilityElement* native_accessibility =
      root_platform_node_delegate->GetNativeViewAccessible();
  std::string value = [native_accessibility.accessibilityValue UTF8String];
  EXPECT_TRUE(value == "accessibility");
  EXPECT_EQ(native_accessibility.accessibilityRole, NSAccessibilityStaticTextRole);
  EXPECT_EQ([native_accessibility.accessibilityChildren count], 0u);
  [engine shutDownEngine];
}

TEST(FlutterPlatformNodeDelegateMac, SelectableTextHasCorrectSemantics) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  engine.semanticsEnabled = YES;
  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{.is_text_field = true, .is_read_only = true};
  root.id = 0;
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.text_selection_base = 1;
  root.text_selection_extent = 3;
  root.label = "";
  root.hint = "";
  // Selectable text store its text in value
  root.value = "selectable text";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 0;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  bridge->AddFlutterSemanticsNodeUpdate(root);

  bridge->CommitUpdates();

  auto root_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(0).lock();
  // Verify the accessibility attribute matches.
  NSAccessibilityElement* native_accessibility =
      root_platform_node_delegate->GetNativeViewAccessible();
  std::string value = [native_accessibility.accessibilityValue UTF8String];
  EXPECT_EQ(value, "selectable text");
  EXPECT_EQ(native_accessibility.accessibilityRole, NSAccessibilityStaticTextRole);
  EXPECT_EQ([native_accessibility.accessibilityChildren count], 0u);
  NSRange selection = native_accessibility.accessibilitySelectedTextRange;
  EXPECT_EQ(selection.location, 1u);
  EXPECT_EQ(selection.length, 2u);
  std::string selected_text = [native_accessibility.accessibilitySelectedText UTF8String];
  EXPECT_EQ(selected_text, "el");
}

TEST(FlutterPlatformNodeDelegateMac, SelectableTextWithoutSelectionReturnZeroRange) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  engine.semanticsEnabled = YES;
  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{.is_text_field = true, .is_read_only = true};
  root.id = 0;
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.text_selection_base = -1;
  root.text_selection_extent = -1;
  root.label = "";
  root.hint = "";
  // Selectable text store its text in value
  root.value = "selectable text";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 0;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  bridge->AddFlutterSemanticsNodeUpdate(root);

  bridge->CommitUpdates();

  auto root_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(0).lock();
  // Verify the accessibility attribute matches.
  NSAccessibilityElement* native_accessibility =
      root_platform_node_delegate->GetNativeViewAccessible();
  NSRange selection = native_accessibility.accessibilitySelectedTextRange;
  EXPECT_TRUE(selection.location == NSNotFound);
  EXPECT_EQ(selection.length, 0u);
}

// MOCK_ENGINE_PROC is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

TEST(FlutterPlatformNodeDelegateMac, CanPerformAction) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;

  // Attach the view to a NSWindow.
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                                 styleMask:NSBorderlessWindowMask
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.contentView = viewController.view;

  engine.semanticsEnabled = YES;
  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{};
  root.flags2 = &flags;
  root.id = 0;
  root.label = "root";
  root.hint = "";
  root.value = "";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 1;
  int32_t children[] = {1};
  root.children_in_traversal_order = children;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  bridge->AddFlutterSemanticsNodeUpdate(root);

  FlutterSemanticsNode2 child1;
  FlutterSemanticsFlags child_flags = FlutterSemanticsFlags{};
  child1.flags2 = &child_flags;
  child1.id = 1;
  child1.label = "child 1";
  child1.hint = "";
  child1.value = "";
  child1.increased_value = "";
  child1.decreased_value = "";
  child1.tooltip = "";
  child1.child_count = 0;
  child1.custom_accessibility_actions_count = 0;
  child1.identifier = "";
  bridge->AddFlutterSemanticsNodeUpdate(child1);

  bridge->CommitUpdates();

  auto root_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(1).lock();

  // Set up embedder API mock.
  FlutterSemanticsAction called_action;
  uint64_t called_id;

  engine.embedderAPI.DispatchSemanticsAction = MOCK_ENGINE_PROC(
      DispatchSemanticsAction,
      ([&called_id, &called_action](auto engine, uint64_t id, FlutterSemanticsAction action,
                                    const uint8_t* data, size_t data_length) {
        called_id = id;
        called_action = action;
        return kSuccess;
      }));

  // Performs an AXAction.
  ui::AXActionData action_data;
  action_data.action = ax::mojom::Action::kDoDefault;
  root_platform_node_delegate->AccessibilityPerformAction(action_data);

  EXPECT_EQ(called_action, FlutterSemanticsAction::kFlutterSemanticsActionTap);
  EXPECT_EQ(called_id, 1u);

  [engine setViewController:nil];
  [engine shutDownEngine];
}

// NOLINTEND(clang-analyzer-core.StackAddressEscape)

TEST(FlutterPlatformNodeDelegateMac, TextFieldUsesFlutterTextField) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  [viewController loadView];

  // Unit test localization is unnecessary.
  // NOLINTNEXTLINE(clang-analyzer-optin.osx.cocoa.localizability.NonLocalizedStringChecker)
  engine.textInputPlugin.string = @"textfield";
  // Creates a NSWindow so that the native text field can become first responder.
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                                 styleMask:NSBorderlessWindowMask
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.contentView = viewController.view;
  engine.semanticsEnabled = YES;

  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{0};
  FlutterSemanticsFlags child_flags = FlutterSemanticsFlags{.is_text_field = true};
  root.id = 0;
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.label = "root";
  root.hint = "";
  root.value = "";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 1;
  int32_t children[] = {1};
  root.children_in_traversal_order = children;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  root.rect = {0, 0, 100, 100};  // LTRB
  root.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(root);

  double rectSize = 50;
  double transformFactor = 0.5;

  FlutterSemanticsNode2 child1;
  child1.id = 1;
  child1.flags2 = &child_flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  child1.actions = static_cast<FlutterSemanticsAction>(0);
  child1.label = "";
  child1.hint = "";
  child1.value = "textfield";
  child1.increased_value = "";
  child1.decreased_value = "";
  child1.tooltip = "";
  child1.text_selection_base = -1;
  child1.text_selection_extent = -1;
  child1.child_count = 0;
  child1.custom_accessibility_actions_count = 0;
  child1.identifier = "";
  child1.rect = {0, 0, rectSize, rectSize};  // LTRB
  child1.transform = {transformFactor, 0, 0, 0, transformFactor, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(child1);

  bridge->CommitUpdates();

  auto child_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(1).lock();
  // Verify the accessibility attribute matches.
  id native_accessibility = child_platform_node_delegate->GetNativeViewAccessible();
  EXPECT_EQ([native_accessibility isKindOfClass:[FlutterTextField class]], YES);
  FlutterTextField* native_text_field = (FlutterTextField*)native_accessibility;

  NSView* view = viewController.flutterView;
  CGRect scaledBounds = [view convertRectToBacking:view.bounds];
  CGSize scaledSize = scaledBounds.size;
  double pixelRatio = view.bounds.size.width == 0 ? 1 : scaledSize.width / view.bounds.size.width;

  double expectedFrameSize = rectSize * transformFactor / pixelRatio;
  EXPECT_EQ(NSEqualRects(native_text_field.frame, NSMakeRect(0, 600 - expectedFrameSize,
                                                             expectedFrameSize, expectedFrameSize)),
            YES);

  [native_text_field startEditing];
  EXPECT_EQ([native_text_field.stringValue isEqualToString:@"textfield"], YES);
}

// Keyboard focus reaching a text field (a click, Tab) makes the field the
// field editor's client, as VoiceOver focusing it does. Before, the field
// editor stayed first responder on its own, outside the field, and was the
// focused accessibility element: an unnamed text area whose parent was the
// window, so VoiceOver's next move left the window's content.
TEST(FlutterPlatformNodeDelegateMac, KeyboardFocusOnTextFieldStartsEditing) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  [viewController loadView];

  // Creates a NSWindow so that the native text field can become first responder.
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                                 styleMask:NSBorderlessWindowMask
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.contentView = viewController.view;
  engine.semanticsEnabled = YES;

  auto bridge = viewController.accessibilityBridge.lock();
  FlutterSemanticsNode2 root;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{0};
  FlutterSemanticsFlags child_flags = FlutterSemanticsFlags{
      .is_focused = FlutterTristate::kFlutterTristateTrue, .is_text_field = true};
  root.id = 0;
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.label = "root";
  root.hint = "";
  root.value = "";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 1;
  int32_t children[] = {1};
  root.children_in_traversal_order = children;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  root.rect = {0, 0, 100, 100};  // LTRB
  root.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(root);

  FlutterSemanticsNode2 child1;
  child1.id = 1;
  child1.flags2 = &child_flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  child1.actions = static_cast<FlutterSemanticsAction>(0);
  child1.label = "Search";
  child1.hint = "";
  child1.value = "";
  child1.increased_value = "";
  child1.decreased_value = "";
  child1.tooltip = "";
  child1.text_selection_base = -1;
  child1.text_selection_extent = -1;
  child1.child_count = 0;
  child1.custom_accessibility_actions_count = 0;
  child1.identifier = "";
  child1.rect = {0, 0, 50, 50};  // LTRB
  child1.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(child1);

  bridge->CommitUpdates();

  auto root_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(0).lock();
  auto child_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(1).lock();
  id native_accessibility = child_platform_node_delegate->GetNativeViewAccessible();
  ASSERT_TRUE([native_accessibility isKindOfClass:[FlutterTextField class]]);
  FlutterTextField* native_text_field = (FlutterTextField*)native_accessibility;

  EXPECT_EQ(engine.textInputPlugin.client, native_text_field);
  // The field sits in the tree: its parent is the root's element, which
  // lists it among its children.
  EXPECT_EQ([native_text_field accessibilityParent],
            root_platform_node_delegate -> GetNativeViewAccessible());
  [engine shutDownEngine];
}

TEST(FlutterPlatformNodeDelegateMac, ChangingFlagsUpdatesNativeViewAccessible) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  [viewController loadView];

  // Creates a NSWindow so that the native text field can become first responder.
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                                 styleMask:NSBorderlessWindowMask
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.contentView = viewController.view;
  engine.semanticsEnabled = YES;

  auto bridge = viewController.accessibilityBridge.lock();
  // Initialize ax node data.
  FlutterSemanticsNode2 root;
  root.id = 0;
  FlutterSemanticsFlags flags = FlutterSemanticsFlags{0};
  root.flags2 = &flags;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  root.actions = static_cast<FlutterSemanticsAction>(0);
  root.label = "root";
  root.hint = "";
  root.value = "";
  root.increased_value = "";
  root.decreased_value = "";
  root.tooltip = "";
  root.child_count = 1;
  int32_t children[] = {1};
  root.children_in_traversal_order = children;
  root.custom_accessibility_actions_count = 0;
  root.identifier = "";
  root.rect = {0, 0, 100, 100};  // LTRB
  root.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(root);

  double rectSize = 50;
  double transformFactor = 0.5;

  FlutterSemanticsNode2 child1;
  FlutterSemanticsFlags child_flags = FlutterSemanticsFlags{0};
  child1.flags2 = &child_flags;
  child1.id = 1;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  child1.actions = static_cast<FlutterSemanticsAction>(0);
  child1.label = "";
  child1.hint = "";
  child1.value = "textfield";
  child1.increased_value = "";
  child1.decreased_value = "";
  child1.tooltip = "";
  child1.text_selection_base = -1;
  child1.text_selection_extent = -1;
  child1.child_count = 0;
  child1.custom_accessibility_actions_count = 0;
  child1.identifier = "";
  child1.rect = {0, 0, rectSize, rectSize};  // LTRB
  child1.transform = {transformFactor, 0, 0, 0, transformFactor, 0, 0, 0, 1};
  bridge->AddFlutterSemanticsNodeUpdate(child1);

  bridge->CommitUpdates();

  auto child_platform_node_delegate = bridge->GetFlutterPlatformNodeDelegateFromID(1).lock();
  // Verify the accessibility attribute matches.
  id native_accessibility = child_platform_node_delegate->GetNativeViewAccessible();
  EXPECT_TRUE([[native_accessibility className] isEqualToString:@"AXPlatformNodeCocoa"]);

  // Converting child to text field should produce `FlutterTextField` native view accessible.

  FlutterSemanticsFlags child_flags_updated_1 = FlutterSemanticsFlags{.is_text_field = true};
  child1.flags2 = &child_flags_updated_1;
  bridge->AddFlutterSemanticsNodeUpdate(child1);
  bridge->CommitUpdates();

  native_accessibility = child_platform_node_delegate->GetNativeViewAccessible();
  EXPECT_TRUE([native_accessibility isKindOfClass:[FlutterTextField class]]);

  FlutterSemanticsFlags child_flags_updated_2 = FlutterSemanticsFlags{.is_text_field = false};
  child1.flags2 = &child_flags_updated_2;
  bridge->AddFlutterSemanticsNodeUpdate(child1);
  bridge->CommitUpdates();

  native_accessibility = child_platform_node_delegate->GetNativeViewAccessible();
  EXPECT_TRUE([[native_accessibility className] isEqualToString:@"AXPlatformNodeCocoa"]);
}

// A control that is off (hasEnabledState without isEnabled) reaches macOS as
// not enabled (AXEnabled = NO), so VoiceOver reads it as "dimmed". Before the
// accessibility bridge set the node's restriction, every Flutter button and
// checkbox on the Mac reported AXEnabled = YES, whatever the app said.
TEST(FlutterPlatformNodeDelegateMac, DisabledControlsAreNotAccessibilityEnabled) {
  FlutterViewController* viewController = CreateTestViewController();
  FlutterEngine* engine = viewController.engine;
  [viewController loadView];
  engine.semanticsEnabled = YES;
  auto bridge = viewController.accessibilityBridge.lock();

  FlutterSemanticsFlags root_flags = FlutterSemanticsFlags{};
  FlutterSemanticsFlags disabled_button_flags = FlutterSemanticsFlags{
      .is_enabled = FlutterTristate::kFlutterTristateFalse, .is_button = true};
  FlutterSemanticsFlags enabled_button_flags =
      FlutterSemanticsFlags{.is_enabled = FlutterTristate::kFlutterTristateTrue, .is_button = true};
  FlutterSemanticsFlags disabled_checkbox_flags =
      FlutterSemanticsFlags{.is_checked = FlutterCheckState::kFlutterCheckStateTrue,
                            .is_enabled = FlutterTristate::kFlutterTristateFalse};

  auto add_node = [&bridge](int32_t id, const char* label, FlutterSemanticsFlags* flags,
                            FlutterSemanticsAction actions, size_t child_count,
                            const int32_t* children) {
    FlutterSemanticsNode2 node = {};
    node.id = id;
    node.flags2 = flags;
    node.actions = actions;
    node.text_selection_base = -1;
    node.text_selection_extent = -1;
    node.label = label;
    node.hint = "";
    node.value = "";
    node.increased_value = "";
    node.decreased_value = "";
    node.tooltip = "";
    node.child_count = child_count;
    node.children_in_traversal_order = children;
    node.custom_accessibility_actions_count = 0;
    node.identifier = "";
    node.rect = {0, 0, 100, 100};  // LTRB
    node.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bridge->AddFlutterSemanticsNodeUpdate(node);
  };
  int32_t children[] = {1, 2, 3};
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  add_node(0, "root", &root_flags, static_cast<FlutterSemanticsAction>(0), 3, children);
  add_node(1, "Ignore untagged faces", &disabled_button_flags,
           static_cast<FlutterSemanticsAction>(0), 0, nullptr);
  add_node(2, "Rescan faces", &enabled_button_flags,
           FlutterSemanticsAction::kFlutterSemanticsActionTap, 0, nullptr);
  add_node(3, "Info", &disabled_checkbox_flags, static_cast<FlutterSemanticsAction>(0), 0, nullptr);
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  bridge->CommitUpdates();

  id disabled_button =
      bridge->GetFlutterPlatformNodeDelegateFromID(1).lock()->GetNativeViewAccessible();
  ASSERT_NE(disabled_button, nil);
  EXPECT_TRUE([[disabled_button accessibilityRole] isEqualToString:NSAccessibilityButtonRole]);
  EXPECT_FALSE([disabled_button isAccessibilityEnabled]);

  id enabled_button =
      bridge->GetFlutterPlatformNodeDelegateFromID(2).lock()->GetNativeViewAccessible();
  ASSERT_NE(enabled_button, nil);
  EXPECT_TRUE([[enabled_button accessibilityRole] isEqualToString:NSAccessibilityButtonRole]);
  EXPECT_TRUE([enabled_button isAccessibilityEnabled]);

  id disabled_checkbox =
      bridge->GetFlutterPlatformNodeDelegateFromID(3).lock()->GetNativeViewAccessible();
  ASSERT_NE(disabled_checkbox, nil);
  EXPECT_TRUE([[disabled_checkbox accessibilityRole] isEqualToString:NSAccessibilityCheckBoxRole]);
  EXPECT_FALSE([disabled_checkbox isAccessibilityEnabled]);

  [engine shutDownEngine];
}

}  // namespace flutter::testing
