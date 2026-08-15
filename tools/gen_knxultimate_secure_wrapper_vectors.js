/*
Generates KNXnet/IP SecureWrapper (0x0950) vector files using KNXUltimate's
reference primitives.

This is a developer utility; vector files are checked in under
`test/vectors/knxnetip_secure_wrapper/`.
*/

const fs = require('fs')
const path = require('path')

const {
	calculateMessageAuthenticationCodeCBC,
	encryptDataCtr,
} = require('../external/KNXUltimate/src/secure/security_primitives')

const {
	KNXIP_HDR_SECURE_WRAPPER,
	SECURE_WRAPPER_TAG,
	SECURE_WRAPPER_CTR_SUFFIX,
	SECURE_WRAPPER_OVERHEAD,
	MAC_LEN_FULL,
	SECURE_SEQ_LEN,
} = require('../external/KNXUltimate/src/secure/secure_knx_constants')

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

	const macCbc = calculateMessageAuthenticationCodeCBC(key, additionalData, inner, block0)
	if (macCbc.length !== MAC_LEN_FULL) throw new Error('bad mac length')

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

	return { hdr, macCbc, encMac, encData, frame }
}

function main() {
	const outDir = path.join(__dirname, '..', 'test', 'vectors', 'knxnetip_secure_wrapper')

	// A plausible inner KNXnet/IP TUNNELING_REQUEST frame (0x0420) with 14 bytes of cEMI-ish payload.
	const inner = Buffer.from(
		'061004200018' +
			'04112200' +
			'2900bce0110a230100800100000000',
		'hex',
	)

	// ===== Secure tunnelling wrapper (sid != 0, tag fixed 0000, ctr suffix fixed 0000ff00)
	{
		const key = Buffer.from('00112233445566778899aabbccddeeff', 'hex')
		const sessionId = 0x5100
		const seq = Buffer.from('000000000001', 'hex')
		const serial = Buffer.from('010203040506', 'hex')
		const tag = Buffer.from(SECURE_WRAPPER_TAG)
		const counterSuffix = Buffer.from(SECURE_WRAPPER_CTR_SUFFIX)

		const { hdr, macCbc, encMac, frame } = buildSecureWrapper({
			key,
			sessionId,
			seq,
			serial,
			tag,
			inner,
			counterSuffix,
		})

		writeVec(
			path.join(outDir, 'secure_wrapper_0950_tunnel.vec'),
			{
				name: 'secure_wrapper_0950_tunnel',
				key: hex(key),
				sid: hex(Buffer.from([(sessionId >> 8) & 0xff, sessionId & 0xff])),
				seq: hex(seq),
				serial: hex(serial),
				tag: hex(tag),
				hdr: hex(hdr),
				inner: hex(inner),
				mac_cbc: hex(macCbc),
				mac_enc: hex(encMac),
				frame: hex(frame),
			},
			[
				'KNXnet/IP Secure Wrapper (0x0950) tunnelling vector generated from KNXUltimate primitives.',
				'sessionId != 0; tag is fixed 0x0000; CTR suffix is 0000ff00.',
			],
		)
	}

	// ===== Secure routing wrapper (sid == 0, tag chosen, ctr suffix tag||ff00)
	{
		const key = Buffer.from('6da809eed8dbdbd0a0680fb88afe2bb2', 'hex') // same as TimerNotify vector backbone key
		const sessionId = 0x0000
		const seq = Buffer.from('000000ca0279', 'hex') // reuse TimerNotify timer for determinism
		const serial = Buffer.from('00a207164012', 'hex')
		const tag = Buffer.from('6f8c', 'hex')
		const counterSuffix = Buffer.from([tag[0], tag[1], 0xff, 0x00])

		const { hdr, macCbc, encMac, frame } = buildSecureWrapper({
			key,
			sessionId,
			seq,
			serial,
			tag,
			inner,
			counterSuffix,
		})

		writeVec(
			path.join(outDir, 'secure_wrapper_0950_routing.vec'),
			{
				name: 'secure_wrapper_0950_routing',
				key: hex(key),
				sid: hex(Buffer.from([0x00, 0x00])),
				seq: hex(seq),
				serial: hex(serial),
				tag: hex(tag),
				hdr: hex(hdr),
				inner: hex(inner),
				mac_cbc: hex(macCbc),
				mac_enc: hex(encMac),
				frame: hex(frame),
			},
			[
				'KNXnet/IP Secure Wrapper (0x0950) routing vector generated from KNXUltimate primitives.',
				'sessionId == 0; tag is explicit; CTR suffix is tag||ff00.',
			],
		)
	}

	console.log(`Wrote vectors into ${outDir}`)
}

main()
