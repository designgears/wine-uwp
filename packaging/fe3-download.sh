#!/usr/bin/env bash
# Download a package from Microsoft's update servers (FE3) by its update id, as the Store does for a product's
# exact version. FE3 keeps serving old UWP packages that the Store no longer offers.
# Usage: packaging/fe3-download.sh UPDATE_ID OUT [REVISION]
#
# Minecraft for Windows' update ids, per version: MCMrARM's mc-w10-versiondb (packaging/install-minecraft-uwp.sh
# looks them up). The signed CDN link FE3 hands out expires after a few hours, so ask again for each download.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 UPDATE_ID OUT [REVISION]" >&2; exit 2; }
ID=$1 OUT=$2 REV=${3:-1}
TO=https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx/secured
NOW=$(date -u +%Y-%m-%dT%H:%M:%S.000Z); EXP=$(date -u -d '+5 min' +%Y-%m-%dT%H:%M:%S.000Z)
# the request appxinstall's store.c sends (GetExtendedUpdateInfo2, its device attributes)
BODY='<s:Envelope xmlns:a="http://www.w3.org/2005/08/addressing" xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Header><a:Action s:mustUnderstand="1">http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService/GetExtendedUpdateInfo2</a:Action><a:MessageID>urn:uuid:'$(cat /proc/sys/kernel/random/uuid)'</a:MessageID><a:To s:mustUnderstand="1">'$TO'</a:To><o:Security s:mustUnderstand="1" xmlns:o="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"><Timestamp xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><Created>'$NOW'</Created><Expires>'$EXP'</Expires></Timestamp><wuws:WindowsUpdateTicketsToken wsu:id="ClientMSA" xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd" xmlns:wuws="http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization"><TicketType Name="MSA" Version="1.0" Policy="MBI_SSL"></TicketType></wuws:WindowsUpdateTicketsToken></o:Security></s:Header><s:Body><GetExtendedUpdateInfo2 xmlns="http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService"><updateIDs><UpdateIdentity><UpdateID>'$ID'</UpdateID><RevisionNumber>'$REV'</RevisionNumber></UpdateIdentity></updateIDs><infoTypes><XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType></infoTypes><deviceAttributes>BranchReadinessLevel=CB;CurrentBranch=ge_release;OEMModel=Virtual Machine;FlightRing=Retail;AttrDataVer=21;SystemManufacturer=Microsoft Corporation;InstallLanguage=en-US;OSUILocale=en-US;InstallationType=Client;FlightingBranchName=external;OSSkuId=48;FlightContent=Branch;App=WU;AppVer=10.0.26100.1;OSArchitecture=AMD64;IsFlightingEnabled=0;IsDeviceRetailDemo=0;TelemetryLevel=3;OSVersion=10.0.26100.1;DeviceFamily=Windows.Desktop;</deviceAttributes></GetExtendedUpdateInfo2></s:Body></s:Envelope>'
# FE3 uses Microsoft's own CA, absent from most Linux trust stores, hence -k for this one request; the package
# comes from Microsoft's CDN and the installer checks it against its block map.
URL=$(curl -ksS -H 'Content-Type: application/soap+xml; charset=utf-8' --data-binary "$BODY" "$TO" \
      | grep -o '<Url>[^<]*' | sed 's/<Url>//;s/&amp;/\&/g' | grep 'P1=' | head -1)
[ -n "$URL" ] || { echo "FE3 has no download for update $ID" >&2; exit 1; }
curl -fL -C - -o "$OUT" "$URL"
unzip -l "$OUT" AppxManifest.xml >/dev/null 2>&1 || unzip -l "$OUT" AppxMetadata/AppxBundleManifest.xml >/dev/null
echo "$OUT"
