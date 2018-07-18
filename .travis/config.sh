#!/bin/sh

CANARY_JSON=$(curl -s https://raw.githubusercontent.com/topjohnwu/magisk_files/master/canary_builds/canary.json)
CANARY_APK_VERSION=$(echo $CANARY_JSON | jq -r '.app.version' | awk -F "-" '{print $1}')-${GIT_COMMIT_HASH}
CANARY_APK_VER_CODE=$(echo $CANARY_JSON | jq -r '.app.versionCode')
CANARY_MAGISK_VERSION=$(echo $CANARY_JSON | jq -r '.magisk.version'| awk -F "-" '{print $1}')-${GIT_COMMIT_HASH}
CANARY_MAGISK_VER_CODE=$(echo $CANARY_JSON | jq -r '.magisk.versionCode')
sed -e "s/version=.*/version=${CANARY_MAGISK_VERSION}/" \
 -e "s/versionCode=.*/versionCode=${CANARY_MAGISK_VER_CODE}/" \
 -e "s/appVersion=.*/appVersion=${CANARY_APK_VERSION}/" \
 -e "s/appVersionCode=.*/appVersionCode=${CANARY_APK_VER_CODE}/" \
 -e "s/prettyName=.*/prettyName=true/" \
 -e "s/keyStorePass=.*/keyStorePass=android/" \
 -e "s/keyAlias=.*/keyAlias=android/" \
 -e "s/keyPass=.*/keyPass=android/" \
 config.prop.sample > config.prop