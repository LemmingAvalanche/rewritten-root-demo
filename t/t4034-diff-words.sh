#!/bin/sh

test_description='word diff colors'

. ./test-lib.sh

test_expect_success setup '

	git config diff.color.old red
	git config diff.color.new green

'

decrypt_color () {
	sed \
		-e 's/.\[1m/<WHITE>/g' \
		-e 's/.\[31m/<RED>/g' \
		-e 's/.\[32m/<GREEN>/g' \
		-e 's/.\[36m/<BROWN>/g' \
		-e 's/.\[m/<RESET>/g'
}

word_diff () {
	test_must_fail git diff --no-index "$@" pre post > output &&
	decrypt_color < output > output.decrypted &&
	test_cmp expect output.decrypted
}

cat > pre <<\EOF
h(4)

a = b + c
EOF

cat > post <<\EOF
h(4),hh[44]

a = b + c

aa = a

aeff = aeff * ( aaa )
EOF

cat > expect <<\EOF
<WHITE>diff --git a/pre b/post<RESET>
<WHITE>index 330b04f..5ed8eff 100644<RESET>
<WHITE>--- a/pre<RESET>
<WHITE>+++ b/post<RESET>
<BROWN>@@ -1,3 +1,7 @@<RESET>
<RED>h(4)<RESET><GREEN>h(4),hh[44]<RESET>
<RESET>
a = b + c<RESET>

<GREEN>aa = a<RESET>

<GREEN>aeff = aeff * ( aaa )<RESET>
EOF

test_expect_success 'word diff with runs of whitespace' '

	word_diff --color-words

'

test_done
