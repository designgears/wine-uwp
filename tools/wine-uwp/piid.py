#!/usr/bin/env python
"""Compute WinRT parameterised interface IIDs (PIIDs).

A shim that has to answer QueryInterface for IAsyncOperation<T>, IVector<T>, IObservableVector<T>
and friends needs those IIDs, and they are not in any .winmd or in HKCR\\Interface: they are
derived. This does the derivation, so a shim does not have to guess.

    piid.py 'IAsyncOperation<Windows.Storage.StorageLibrary;{1edd7103-0e5e-4d6c-b5e8-9318983d6a03}>'
    piid.py 'IAsyncOperation<enum Windows.Storage.Search.IndexedState>'
    piid.py --signature 'pinterface({9fc2b0bb-...};rc(Foo;{...}))'
    piid.py --self-test

The IID is SHA-1 over the WinRT namespace GUID {11f47ad5-7b73-42c0-abae-878b1e16adee} followed by
the UTF-8 type signature, truncated to 16 bytes with the UUID version set to 5. The signature of a
runtime class is rc(<full name>;{<default interface IID>}) - the *default* interface, which is the
first one listed on the class in metadata. An enum is enum(<full name>;i4), written here as
"enum <full name>"; enums with an unsigned underlying type would be u4, which nothing has needed
yet.

--self-test checks the implementation against three IIDs widl generates into
include/windows.storage.h, so a wrong answer here shows up immediately rather than as an
E_NOINTERFACE three hours later.
"""
import hashlib, sys, uuid

WINRT_NS = uuid.UUID("11f47ad5-7b73-42c0-abae-878b1e16adee").bytes

# The generic interfaces' own IIDs, which parameterise into the signature.
GENERIC = {
    "IAsyncOperation":                  "9fc2b0bb-e446-44e2-aa61-9cab8f636af2",
    "IAsyncOperationCompletedHandler":  "fcdcf02c-e5d8-4478-915a-4d90b74b83a5",
    "IAsyncOperationWithProgress":      "b5d036d7-e297-498f-ba60-0289e76e23dd",
    "IVector":                          "913337e9-11a1-4345-a3a2-4e7f956e222d",
    "IVectorView":                      "bbe1fa4c-b0e3-4583-baef-1f1b2e483e56",
    "IObservableVector":                "5917eb53-50b4-4a0d-b309-65862b3f1dbc",
    "IIterable":                        "faa585ea-6214-4217-afda-7f46de5869b3",
    "IIterator":                        "6a79e863-4300-459a-9966-cbb660963ee1",
    "IMap":                             "3c2925fe-8519-45c1-aa79-197b6718c1c1",
    "IMapView":                         "e480ce40-a338-4ada-adcf-272272e48cb9",
    "IReference":                       "61c17706-2d65-11e0-9ae8-d48564015472",
    "IEventHandler":                    "9de1c535-6ae1-11e0-84e1-18a905bcc53f",
    "ITypedEventHandler":               "9de1c534-6ae1-11e0-84e1-18a905bcc53f",
}


def piid(signature):
    digest = bytearray(hashlib.sha1(WINRT_NS + signature.encode()).digest()[:16])
    digest[6] = (digest[6] & 0x0f) | 0x50   # version 5
    digest[8] = (digest[8] & 0x3f) | 0x80   # RFC 4122 variant
    return str(uuid.UUID(bytes=bytes(digest)))


def signature_for(generic, arg):
    """generic is a name from GENERIC; arg is "Full.Type.Name;{default-iface-iid}" for a runtime
    class, or "enum Full.Type.Name" for an enum."""
    arg = arg.strip()
    if arg.startswith("enum "):
        return "pinterface({%s};enum(%s;i4))" % (GENERIC[generic], arg[5:].strip())
    name, _, iid = arg.partition(";")
    return "pinterface({%s};rc(%s;%s))" % (GENERIC[generic], name.strip(), iid.strip())


SELF_TEST = [
    # (generic, "class;{default interface iid}", IID widl puts in include/windows.storage.h)
    ("IAsyncOperation", "Windows.Storage.StorageFolder;{72d1cb78-b3ef-4f75-a80b-6fd9dae2944b}",
     "6be9e7d7-e83a-5cbc-802c-1768960b52c3"),
    ("IAsyncOperationCompletedHandler", "Windows.Storage.StorageFolder;{72d1cb78-b3ef-4f75-a80b-6fd9dae2944b}",
     "c211026e-9e63-5452-ba54-3a07d6a96874"),
    ("IVectorView", "Windows.Storage.StorageFolder;{72d1cb78-b3ef-4f75-a80b-6fd9dae2944b}",
     "e20debc6-dc4e-542e-a2e7-a24d19c8dd62"),
    # enum arguments take a different signature form, so check one of those too
    ("IAsyncOperation", "enum Windows.Devices.Enumeration.DeviceAccessStatus",
     "c00bc2f2-a7f8-5f3f-80d1-2808ef6bca10"),
    ("IAsyncOperation", "enum Windows.Storage.KnownFoldersAccessStatus",
     "d7f094b5-0ea2-5654-85b9-38ee5de90ffa"),
]


def self_test():
    bad = 0
    for generic, arg, want in SELF_TEST:
        got = piid(signature_for(generic, arg))
        ok = got == want
        bad += not ok
        print("%s %s<%s>\n    %s%s" % ("ok  " if ok else "BAD ", generic, arg, got,
                                       "" if ok else "\n    expected " + want))
    return bad


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if args[0] == "--self-test":
        return 1 if self_test() else 0
    if args[0] == "--signature":
        print(piid(args[1]))
        return 0
    spec = args[0]
    if "<" not in spec or not spec.endswith(">"):
        print("expected Generic<Full.Type.Name;{default-iface-iid}> or Generic<enum Full.Type.Name>,"
              " or --signature", file=sys.stderr)
        return 2
    generic, _, rest = spec.partition("<")
    if generic not in GENERIC:
        print("unknown generic %r; known: %s" % (generic, " ".join(sorted(GENERIC))), file=sys.stderr)
        return 2
    print(piid(signature_for(generic, rest[:-1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
