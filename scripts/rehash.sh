#/bin/sh
# adapted from c_rehash

[ -n "$1" ] || { echo "usage: $0 path"; exit 1; }

find $1 -type l | xargs rm -f
cd $1

for c in *.pem; do
	[ "$c" = "*.pem" ] && continue
	hash=$(openssl x509 -noout -hash -in "$c")
	if egrep -q -- '-BEGIN( X509 | TRUSTED | )CERTIFICATE-' "$c"; then
		suf=0
		while [ -e $hash.$suf ]; do suf=$(( $suf + 1 )); done
		ln -s "$c" $hash.$suf
	fi

	if egrep -q -- '-BEGIN X509 CRL-' "$c"; then
		suf=0
		while [ -e $hash.r$suf ]; do suf=$(( $suf + 1 )); done
		ln -s "$c" $hash.r$suf
	fi
done
