/*
Generates KNX/IP Secure session handshake vectors (0x0951..0x0954) using
Node's built-in crypto.

Run (from repo root):
	node tools/gen_knxultimate_secure_session_vectors.js

Vector files are written under:
	test/vectors/knxnetip_secure_session/
*/

const fs = require('fs')
const path = require('path')
const crypto = require('crypto')

// Constants copied from KNXUltimate's secure_knx_constants.ts
const KNXIP_HDR_SECURE_WRAPPER = Buffer.from('06100950', 'hex')
const KNXIP_HDR_SECURE_SESSION_REQUEST = Buffer.from('06100951', 'hex')
const KNXIP_HDR_SECURE_SESSION_AUTHENTICATE = Buffer.from('06100953', 'hex')
const SECURE_WRAPPER_TAG = Buffer.from('0000', 'hex')
const SECURE_WRAPPER_CTR_SUFFIX = Buffer.from('0000ff00', 'hex')
const SECURE_WRAPPER_OVERHEAD = 38
const HPAI_CONTROL_ENDPOINT_EMPTY = Buffer.from('0802000000000000', 'hex')
const PUBLIC_KEY_LEN = 32
const SECURE_SEQ_LEN = 6
const MAC_LEN_FULL = 16
const AUTH_CTR_IV = Buffer.from('0000000000000000000000000000ff00', 'hex')

function aes128CbcMac(key, additionalData, payload, block0) {
	if (key.length !== 16) throw new Error('key must be 16 bytes')
	if (block0.length !== 16) throw new Error('block0 must be 16 bytes')
	const aadLen = additionalData.length
	const blocks = Buffer.concat([
		block0,
		Buffer.from([(aadLen >> 8) & 0xff, aadLen & 0xff]),
		additionalData,
		payload,
	])
	const padLen = (16 - (blocks.length % 16)) % 16
	const padded = padLen ? Buffer.concat([blocks, Buffer.alloc(padLen, 0)]) : blocks
	const cipher = crypto.createCipheriv('aes-128-cbc', key, Buffer.alloc(16, 0))
	cipher.setAutoPadding(false)
	const enc = Buffer.concat([cipher.update(padded), cipher.final()])
	return enc.subarray(enc.length - 16)
}

function aes128CtrCrypt(key, counter0, data) {
	const c = crypto.createCipheriv('aes-128-ctr', key, counter0)
	return Buffer.concat([c.update(data), c.final()])
}

function encryptDataCtr(key, ctr0, macCbc, payload) {
	// KNXUltimate order: encrypt MAC first, then payload.
	const cipher = crypto.createCipheriv('aes-128-ctr', key, ctr0)
	const encMac = cipher.update(macCbc)
	const encPayload = cipher.update(payload)
	return [encPayload, encMac]
}

function hex(buf) {
	return Buffer.from(buf).toString('hex')
}

function writeVec(filePath, kv, headerLines = []) {
	const lines = []
	for (const h of headerLines) lines.push(`# ${h}`)
	lines.push('')
	for (const [k, v] of Object.entries(kv)) lines.push(`${k}=${v}`)
	lines.push('')
	fs.mkdirSync(path.dirname(filePath), { recursive: true })
	fs.writeFileSync(filePath, lines.join('\n'), 'utf8')
}

const X25519_SPKI_PREFIX_DER = Buffer.from('302a300506032b656e032100', 'hex')
const X25519_PKCS8_PREFIX_DER = Buffer.from('302e020100300506032b656e04220420', 'hex')

function x25519KeyFromRawPrivate(priv32) {
	const der = Buffer.concat([X25519_PKCS8_PREFIX_DER, priv32])
	return crypto.createPrivateKey({ key: der, format: 'der', type: 'pkcs8' })
}

function x25519RawPublicFromKey(keyObj) {
	const pubDer = crypto.createPublicKey(keyObj).export({ type: 'spki', format: 'der' })
	return Buffer.from(pubDer).subarray(pubDer.length - 32)
}

function buildSessionRequest(clientPubKey) {
	const bodyLen = HPAI_CONTROL_ENDPOINT_EMPTY.length + PUBLIC_KEY_LEN
	const totalLen = bodyLen + 6
	return Buffer.concat([
		KNXIP_HDR_SECURE_SESSION_REQUEST,
		Buffer.from([(totalLen >> 8) & 0xff, totalLen & 0xff]),
		HPAI_CONTROL_ENDPOINT_EMPTY,
		clientPubKey,
	])
}

function buildSessionResponse(sessionId, serverPubKey) {
	const totalLen = 6 + 2 + PUBLIC_KEY_LEN
	return Buffer.concat([
		Buffer.from('06100952', 'hex'),
		Buffer.from([(totalLen >> 8) & 0xff, totalLen & 0xff]),
		Buffer.from([(sessionId >> 8) & 0xff, sessionId & 0xff]),
		serverPubKey,
	])
}

function buildSessionAuthenticatePlain(userId, userPasswordKey, clientPubKey, serverPubKey) {
	const xor = Buffer.alloc(PUBLIC_KEY_LEN)
	for (let i = 0; i < PUBLIC_KEY_LEN; i++) xor[i] = clientPubKey[i] ^ serverPubKey[i]

	const additionalData = Buffer.concat([
		KNXIP_HDR_SECURE_SESSION_AUTHENTICATE,
		Buffer.from([0x00, 0x18]),
		Buffer.from([0x00, userId]),
		xor,
	])
	const block0 = Buffer.alloc(16, 0)
	const macCbc = aes128CbcMac(userPasswordKey, additionalData, Buffer.alloc(0), block0)
	const ctr = crypto.createCipheriv('aes-128-ctr', userPasswordKey, AUTH_CTR_IV)
	const mac = ctr.update(macCbc)
	return Buffer.concat([
		KNXIP_HDR_SECURE_SESSION_AUTHENTICATE,
		Buffer.from([0x00, 0x18]),
		Buffer.from([0x00, userId]),
		mac,
	])
}

function buildSecureWrapper({ key, sessionId, seq, serial, tag, inner, counterSuffix }) {
	if (seq.length !== SECURE_SEQ_LEN) throw new Error('seq must be 6 bytes')
	if (serial.length !== 6) throw new Error('serial must be 6 bytes')
	if (tag.length !== 2) throw new Error('tag must be 2 bytes')
	if (counterSuffix.length !== 4) throw new Error('counterSuffix must be 4 bytes')

	const totalLen = SECURE_WRAPPER_OVERHEAD + inner.length
	const hdr = Buffer.concat([
		KNXIP_HDR_SECURE_WRAPPER,
		Buffer.from([(totalLen >> 8) & 0xff, totalLen & 0xff]),
	])

	const sidBytes = Buffer.from([(sessionId >> 8) & 0xff, sessionId & 0xff])
	const additionalData = Buffer.concat([hdr, sidBytes])

	const block0 = Buffer.concat([
		seq,
		serial,
		tag,
		Buffer.from([(inner.length >> 8) & 0xff, inner.length & 0xff]),
	])

	const macCbc = aes128CbcMac(key, additionalData, inner, block0)
	const ctr0 = Buffer.concat([seq, serial, counterSuffix])
	const [encData, encMac] = encryptDataCtr(key, ctr0, macCbc, inner)
	if (encMac.length !== MAC_LEN_FULL) throw new Error('bad enc mac length')

	const frame = Buffer.concat([
		hdr,
		sidBytes,
		seq,
		serial,
		tag,
		encData,
		encMac,
	])

	return { hdr, macCbc, encMac, frame }
}

function main() {
	const outDir = path.join(__dirname, '..', 'test', 'vectors', 'knxnetip_secure_session')

	// Deterministic X25519 keys
	const clientPriv = Buffer.from('4f3b1b2a3c4d5e6f708192a3b4c5d6e7f80112233445566778899aabbccddeeff', 'hex')
	const serverPriv = Buffer.from('9f1e2d3c4b5a69788796a5b4c3d2e1f0ffeeddccbbaa99887766554433221100', 'hex')

	const clientPrivObj = x25519KeyFromRawPrivate(clientPriv)
	const serverPrivObj = x25519KeyFromRawPrivate(serverPriv)
	const clientPub = x25519RawPublicFromKey(clientPrivObj)
	const serverPub = x25519RawPublicFromKey(serverPrivObj)

	const secret = crypto.diffieHellman({ privateKey: clientPrivObj, publicKey: crypto.createPublicKey(serverPrivObj) })
	const sessionKey = crypto.createHash('sha256').update(secret).digest().subarray(0, 16)

	// User password -> key
	const userId = 0x11
	const passwordLatin1 = Buffer.from('password', 'latin1')
	const userPasswordKey = crypto.pbkdf2Sync(
		passwordLatin1,
		Buffer.from('user-password.1.secure.ip.knx.org', 'latin1'),
		65536,
		16,
		'sha256',
	)

	const sessionId = 0x1234
	const seq = Buffer.from('000000000001', 'hex')
	const serial = Buffer.from('010203040506', 'hex')
	const tag = Buffer.from(SECURE_WRAPPER_TAG)
	const counterSuffix = Buffer.from(SECURE_WRAPPER_CTR_SUFFIX)

	const req = buildSessionRequest(clientPub)
	const resp = buildSessionResponse(sessionId, serverPub)
	const authPlain = buildSessionAuthenticatePlain(userId, userPasswordKey, clientPub, serverPub)

	const { frame: authWrapped } = buildSecureWrapper({
		key: sessionKey,
		sessionId,
		seq,
		serial,
		tag,
		inner: authPlain,
		counterSuffix,
	})

	writeVec(
		path.join(outDir, 'secure_session_0951_request.vec'),
		{
			name: 'secure_session_0951_request',
			client_priv: hex(clientPriv),
			client_pub: hex(clientPub),
			frame: hex(req),
		},
		['KNX/IP Secure session request (0x0951) vector (deterministic).'],
	)

	writeVec(
		path.join(outDir, 'secure_session_0952_response.vec'),
		{
			name: 'secure_session_0952_response',
			sid: hex(Buffer.from([(sessionId >> 8) & 0xff, sessionId & 0xff])),
			server_priv: hex(serverPriv),
			server_pub: hex(serverPub),
			frame: hex(resp),
		},
		['KNX/IP Secure session response (0x0952) vector (deterministic).'],
	)

	writeVec(
		path.join(outDir, 'secure_session_0953_authenticate_plain.vec'),
		{
			name: 'secure_session_0953_authenticate_plain',
			user_id: hex(Buffer.from([userId])),
			password_latin1: hex(passwordLatin1),
			user_password_key: hex(userPasswordKey),
			client_pub: hex(clientPub),
			server_pub: hex(serverPub),
			frame: hex(authPlain),
		},
		['KNX/IP Secure session authenticate (0x0953) plaintext vector (deterministic).'],
	)

	writeVec(
		path.join(outDir, 'secure_session_0953_authenticate_wrapped.vec'),
		{
			name: 'secure_session_0953_authenticate_wrapped',
			session_key: hex(sessionKey),
			sid: hex(Buffer.from([(sessionId >> 8) & 0xff, sessionId & 0xff])),
			seq: hex(seq),
			serial: hex(serial),
			tag: hex(tag),
			inner: hex(authPlain),
			frame: hex(authWrapped),
		},
		['KNX/IP Secure session authenticate wrapped in SecureWrapper (0x0950).'],
	)

	console.log(`Wrote vectors into ${outDir}`)
}

main()
