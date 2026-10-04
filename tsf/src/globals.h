// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_GLOBALS_H_
#define CXXIME_TSF_GLOBALS_H_

#include "pch.h"

#include <cxxime/text_service_profile.h>

inline constexpr CLSID c_clsidTextService = cxxime::kTextServiceClsid;
inline constexpr GUID c_guidProfile = cxxime::kTextServiceProfileGuid;

// {8EE6B254-FD78-4881-8F4D-DAC5ED403F7A}
DEFINE_GUID(c_guidDisplayAttribute, 0x8ee6b254, 0xfd78, 0x4881, 0x8f, 0x4d, 0xda, 0xc5, 0xed, 0x40, 0x3f, 0x7a);

// {5FE4B213-6C51-4C8F-948D-970B5CEB3106}
DEFINE_GUID(c_guidConvertedDisplayAttribute, 0x5fe4b213, 0x6c51, 0x4c8f, 0x94, 0x8d, 0x97,
            0x0b, 0x5c, 0xeb, 0x31, 0x06);

// {75BEE0B9-7082-4169-A8A3-47AA840A3F30}
DEFINE_GUID(c_guidFocusedDisplayAttribute, 0x75bee0b9, 0x7082, 0x4169, 0xa8, 0xa3, 0x47, 0xaa,
            0x84, 0x0a, 0x3f, 0x30);

// {686527DD-31CD-4B46-A42C-A0393860B458}
DEFINE_GUID(c_guidFocusedConvertedDisplayAttribute, 0x686527dd, 0x31cd, 0x4b46, 0xa4, 0x2c,
            0xa0, 0x39, 0x38, 0x60, 0xb4, 0x58);

// {81E89C76-DFE4-4C90-A7B1-69AEAFE4294B}
DEFINE_GUID(c_guidCandidateUIElement, 0x81e89c76, 0xdfe4, 0x4c90, 0xa7, 0xb1, 0x69, 0xae, 0xaf, 0xe4, 0x29, 0x4b);

// {0C4176C6-EF7B-43DB-AD21-4DD846582F7F}
DEFINE_GUID(c_guidReadingUIElement, 0x0c4176c6, 0xef7b, 0x43db, 0xad, 0x21, 0x4d, 0xd8, 0x46, 0x58, 0x2f, 0x7f);

// {C4238549-F9B9-45FB-9C87-8325C3BB020C}
DEFINE_GUID(c_guidLangBarModeButton, 0xc4238549, 0xf9b9, 0x45fb, 0x9c, 0x87, 0x83, 0x25, 0xc3, 0xbb, 0x02, 0x0c);

// Preserved switch keys (configured shortcuts): Chinese/English, style, punctuation, full/half.
// {A8094513-0D4D-4D35-ACE3-143A2F2DAAF9}
DEFINE_GUID(c_guidPreservedKeyAsciiToggle, 0xa8094513, 0x0d4d, 0x4d35, 0xac, 0xe3, 0x14, 0x3a, 0x2f, 0x2d, 0xaa, 0xf9);
// {13D9F830-2FA8-417C-A0EC-3B73566765C0}
DEFINE_GUID(c_guidPreservedKeyStyle, 0x13d9f830, 0x2fa8, 0x417c, 0xa0, 0xec, 0x3b, 0x73, 0x56, 0x67, 0x65, 0xc0);
// {4353AECB-8568-43FB-AA95-D321BA1DF248}
DEFINE_GUID(c_guidPreservedKeyPunct, 0x4353aecb, 0x8568, 0x43fb, 0xaa, 0x95, 0xd3, 0x21, 0xba, 0x1d, 0xf2, 0x48);
// {0CD763B2-1490-4859-B823-7E118C516F0F}
DEFINE_GUID(c_guidPreservedKeyShape, 0x0cd763b2, 0x1490, 0x4859, 0xb8, 0x23, 0x7e, 0x11, 0x8c, 0x51, 0x6f, 0x0f);

#define TEXTSERVICE_DESC L"知意输入法"
#define TEXTSERVICE_MODEL L"Apartment"
#define TEXTSERVICE_ICON_INDEX 0

#define TEXTSERVICE_LANGID_HANS cxxime::kTextServiceLanguageId

// For Windows 8+
#ifndef TF_IPP_CAPS_IMMERSIVESUPPORT
#define TF_IPP_CAPS_IMMERSIVESUPPORT  0x00010000
#define TF_IPP_CAPS_SYSTRAYSUPPORT    0x00020000
#endif

extern HINSTANCE g_hInst;
extern LONG g_cRefDll;
extern CRITICAL_SECTION g_cs;

void DllAddRef();
void DllRelease();

#endif // CXXIME_TSF_GLOBALS_H_
