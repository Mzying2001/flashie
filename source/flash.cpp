

/* this ALWAYS GENERATED file contains the IIDs and CLSIDs */

/* link this file in with the server and any clients */


 /* File created by MIDL compiler version 8.01.0628 */
/* at Tue Jan 19 11:14:07 2038
 */
/* Compiler settings for IDL8C5B.tmp:
    Oicf, W1, Zp8, env=Win64 (32b run), target_arch=AMD64 8.01.0628 
    protocol : dce , ms_ext, c_ext, robust
    error checks: allocation ref bounds_check enum stub_data 
    VC __declspec() decoration level: 
         __declspec(uuid()), __declspec(selectany), __declspec(novtable)
         DECLSPEC_UUID(), MIDL_INTERFACE()
*/
/* @@MIDL_FILE_HEADING(  ) */

#pragma warning( disable: 4049 )  /* more than 64k source lines */


#ifdef __cplusplus
extern "C"{
#endif 


#include <rpc.h>
#include <rpcndr.h>

#ifdef _MIDL_USE_GUIDDEF_

#ifndef INITGUID
#define INITGUID
#include <guiddef.h>
#undef INITGUID
#else
#include <guiddef.h>
#endif

#define MIDL_DEFINE_GUID(type,name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) \
        DEFINE_GUID(name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8)

#else // !_MIDL_USE_GUIDDEF_

#ifndef __IID_DEFINED__
#define __IID_DEFINED__

typedef struct _IID
{
    unsigned long x;
    unsigned short s1;
    unsigned short s2;
    unsigned char  c[8];
} IID;

#endif // __IID_DEFINED__

#ifndef CLSID_DEFINED
#define CLSID_DEFINED
typedef IID CLSID;
#endif // CLSID_DEFINED

#define MIDL_DEFINE_GUID(type,name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) \
        EXTERN_C __declspec(selectany) const type name = {l,w1,w2,{b1,b2,b3,b4,b5,b6,b7,b8}}

#endif // !_MIDL_USE_GUIDDEF_

MIDL_DEFINE_GUID(IID, LIBID_ShockwaveFlashObjects,0xD27CDB6B,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(IID, IID_IShockwaveFlash,0xD27CDB6C,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(IID, DIID__IShockwaveFlashEvents,0xD27CDB6D,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(IID, IID_IFlashFactory,0xD27CDB70,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(IID, IID_IDispatchEx,0xA6EF9860,0xC720,0x11D0,0x93,0x37,0x00,0xA0,0xC9,0x0D,0xCA,0xA9);


MIDL_DEFINE_GUID(IID, IID_IFlashObjectInterface,0xD27CDB72,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(IID, IID_IServiceProvider,0x6D5140C1,0x7436,0x11CE,0x80,0x34,0x00,0xAA,0x00,0x60,0x09,0xFA);


MIDL_DEFINE_GUID(IID, IID_IFlashObject,0x86230738,0xD762,0x4C50,0xA2,0xDE,0xA7,0x53,0xE5,0xB1,0x68,0x6F);


MIDL_DEFINE_GUID(CLSID, CLSID_ShockwaveFlash,0xD27CDB6E,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(CLSID, CLSID_FlashObjectInterface,0xD27CDB71,0xAE6D,0x11CF,0x96,0xB8,0x44,0x45,0x53,0x54,0x00,0x00);


MIDL_DEFINE_GUID(CLSID, CLSID_FlashObject,0xE0920E11,0x6B65,0x4D5D,0x9C,0x58,0xB1,0xFC,0x5C,0x07,0xDC,0x43);

#undef MIDL_DEFINE_GUID

#ifdef __cplusplus
}
#endif



