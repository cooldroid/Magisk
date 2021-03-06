#!/bin/sh

setup_git() {
  git config --global user.email "${GH_EMAIL}"
  git config --global user.name "${GH_USERNAME}"
}

commit_update_files() {
  MD5=`md5sum out/${MAGISK_ZIP_FILE} | awk '{ print $1 }'`
  git fetch origin +refs/heads/update:refs/remotes/origin/update
  git branch --no-track update refs/remotes/origin/update
  git remote set-branches --add origin update
  git branch --set-upstream-to=origin/update update
  git checkout update
  update_json "\
.app.version |= \"${APK_VER_NAME}\" | .app.versionCode |= \"${APK_VER_CODE}\" | \
.app.link |= \"https://github.com/cooldroid/Magisk/releases/download/${TRAVIS_TAG}/MagiskManager-v${APK_VER_NAME}.apk\" | \
.magisk.version |= \"${MAGISK_VER_STRING}\" | .magisk.versionCode |= \"${MAGISK_VER_CODE}\" | \
.magisk.link |= \"https://github.com/cooldroid/Magisk/releases/download/${TRAVIS_TAG}/${MAGISK_ZIP_FILE}\" | \
.uninstaller.link |= \"https://github.com/cooldroid/Magisk/releases/download/${TRAVIS_TAG}/Magisk-uninstaller-${MAGISK_DATE}.zip\" | \
.magisk.md5 |= \"${MD5}\"\
"
  git add *.json
  git commit --message "Travis build: $TRAVIS_BUILD_NUMBER"
}

update_json() {
  file=custom.json
  jq "$1" $file > $file.tmp && mv -f $file.tmp $file
  cat $file
}

upload_files() {
  git remote set-url origin https://${GH_TOKEN}@github.com/cooldroid/Magisk.git > /dev/null 2>&1
  git push --quiet
}

setup_git
commit_update_files
upload_files
