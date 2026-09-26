// Features.h - Single source of truth for the menu feature list.
// Both Main.cpp GetFeatureList() and MenuUI.cpp read this array.
// Rules: "N_" prefix = explicit featNum, "_True" = default on,
// "CollapseAdd_" = item inside a collapse group,
// Category/Collapse/ButtonLink/RichText* don't consume a featNum.
#ifndef ANDROID_MOD_MENU_FEATURES_H
#define ANDROID_MOD_MENU_FEATURES_H

namespace Features {

static const char* const List[] = {
        "Toggle_No death",
        "Button_Start Invincibility (30 sec duration)",
        "SeekBar_Score multiplier_1_100",
        "SeekBar_Coins multiplier_1_1000",
        "Category_Examples",
        "Toggle_The toggle",
        "100_Toggle_True_The toggle 2",
        "110_Toggle_The toggle 3",
        "SeekBar_The slider_1_100",
        "SeekBar_Kittymemory slider example_1_5",
        "Spinner_The spinner_Items 1,Items 2,Items 3",
        "Button_The button",
        "ButtonLink_The button with link_https://www.youtube.com/",
        "ButtonOnOff_The On/Off button",
        "CheckBox_The Check Box",
        "InputValue_Input number",
        "InputValue_1000_Input number 2",
        "1111_InputLValue_Input long number",
        "InputLValue_1000000000000_Input long number 2",
        "InputText_Input text",
        "RadioButton_Radio buttons_OFF,Mod 1,Mod 2,Mod 3",

        "Collapse_Collapse 1",
        "CollapseAdd_Toggle_The toggle",
        "CollapseAdd_Toggle_The toggle",
        "123_CollapseAdd_Toggle_The toggle",
        "122_CollapseAdd_CheckBox_Check box",
        "CollapseAdd_Button_The button",

        "Collapse_Collapse 2_True",
        "CollapseAdd_SeekBar_The slider_1_100",
        "CollapseAdd_InputValue_Input number",

        "RichTextView_This is text view, not fully HTML."
                "<b>Bold</b> <i>italic</i> <u>underline</u>"
                "<br />New line <font color='red'>Support colors</font>"
                "<br/><big>bigger Text</big>",
        "RichWebView_<html><head><style>body{color: white;}</style></head><body>"
                "This is WebView, with REAL HTML support!"
                "<div style=\"background-color: darkblue; text-align: center;\">Support CSS</div>"
                "<marquee style=\"color: green; font-weight:bold;\" direction=\"left\" scrollamount=\"5\" behavior=\"scroll\">This is <u>scrollable</u> text</marquee>"
                "</body></html>",
};

static constexpr int Count = (int)(sizeof(List) / sizeof(List[0]));

} // namespace Features

#endif // ANDROID_MOD_MENU_FEATURES_H
