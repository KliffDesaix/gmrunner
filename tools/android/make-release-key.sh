#!/bin/sh
# Creates the key Android release builds are signed with, once:
#   tools/android/make-release-key.sh
# It asks for a password and writes, outside the repository:
#   ~/.gmrunner/gmrunner-release.jks       the key (back it up: updates must be signed with it)
#   ~/.gmrunner/keystore.properties        where it is and its password, read by the Gradle build
# GMR_KEY_DIR picks another folder. Needs keytool (JDK 17, e.g. Android Studio's).
set -eu
dir="${GMR_KEY_DIR:-$HOME/.gmrunner}"
jks="$dir/gmrunner-release.jks"
props="$dir/keystore.properties"
[ -e "$jks" ] && { echo "$jks already exists; keep using it (a new key can't update installed apps)" >&2; exit 1; }
keytool="$(command -v keytool || true)"
[ -n "$keytool" ] || keytool="$HOME/Library/Android/jdk17/Contents/Home/bin/keytool"
[ -x "$keytool" ] || { echo "keytool not found (install JDK 17)" >&2; exit 1; }

printf 'Password for the key (6+ characters): '
stty -echo; read -r pass; stty echo; echo
printf 'Again: '
stty -echo; read -r again; stty echo; echo
[ "$pass" = "$again" ] || { echo "the passwords differ" >&2; exit 1; }
[ ${#pass} -ge 6 ] || { echo "at least 6 characters" >&2; exit 1; }
printf 'Name to put in the certificate (e.g. your GitHub name): '
read -r who

mkdir -p "$dir" && chmod 700 "$dir"
GMR_PASS="$pass" "$keytool" -genkeypair -keystore "$jks" -storetype PKCS12 -alias gmrunner -keyalg RSA -keysize 4096 \
    -validity 10000 -dname "CN=${who:-gmRunner}" -storepass:env GMR_PASS -keypass:env GMR_PASS
umask 077
cat > "$props" <<PROPS
storeFile=$jks
storePassword=$pass
keyAlias=gmrunner
keyPassword=$pass
PROPS
chmod 600 "$jks" "$props"
echo "created $jks"
echo "back up $dir (the key and its password) somewhere safe: without them there are no updates"
