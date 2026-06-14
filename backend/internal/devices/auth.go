package devices

import (
	"crypto/sha256"
	"encoding/hex"
)

// hashToken hashes a pre-provisioned device token. The token is a 256-bit
// random value, so plain SHA-256 is sufficient — no need for a slow KDF
// (the protection against brute force is the token's entropy, not its hash).
//
// The hash is what's stored in bbb_devices.auth_token_hash; lookup is done by
// equality on the hashed value, so the plaintext token never reaches the DB.
func hashToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}
