// Copyright TypeDreamMoon. All Rights Reserved.

#include "DreamTextureSha256.h"

namespace
{
	/** FIPS 180-4 第 4.2.2 节：前 64 个素数立方根的小数部分的前 32 位。 */
	constexpr uint32 RoundConstants[64] =
	{
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};

	FORCEINLINE uint32 RotateRight(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32 - Bits));
	}

	/** 吃一个 64 字节的块。字一律按大端读，所以不依赖本机字节序。 */
	void ProcessBlock(uint32 (&State)[8], const uint8* Block)
	{
		uint32 W[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const uint8* P = Block + Index * 4;
			W[Index] = (static_cast<uint32>(P[0]) << 24) | (static_cast<uint32>(P[1]) << 16)
				| (static_cast<uint32>(P[2]) << 8) | static_cast<uint32>(P[3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
			const uint32 S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
			W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
		}

		uint32 A = State[0], B = State[1], C = State[2], D = State[3];
		uint32 E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 Temp1 = H + S1 + Ch + RoundConstants[Index] + W[Index];
			const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temp2 = S0 + Maj;

			H = G;
			G = F;
			F = E;
			E = D + Temp1;
			D = C;
			C = B;
			B = A;
			A = Temp1 + Temp2;
		}

		State[0] += A;
		State[1] += B;
		State[2] += C;
		State[3] += D;
		State[4] += E;
		State[5] += F;
		State[6] += G;
		State[7] += H;
	}
}

void DreamTexture::Sha256(TConstArrayView<uint8> Data, uint8 (&OutDigest)[32])
{
	// FIPS 180-4 第 5.3.3 节：前 8 个素数平方根的小数部分的前 32 位。
	uint32 State[8] =
	{
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	const uint8* Bytes = Data.GetData();
	const uint64 Length = static_cast<uint64>(Data.Num());

	// 整块直接从输入里吃，不先拷贝一份：素材是几 MB 的 PNG，拷一遍纯属浪费。
	const uint64 FullBlocks = Length / 64;
	for (uint64 BlockIndex = 0; BlockIndex < FullBlocks; ++BlockIndex)
	{
		ProcessBlock(State, Bytes + BlockIndex * 64);
	}

	// 尾部：剩余字节 + 0x80 + 补零 + 8 字节大端的比特长度。
	// 剩余 >= 56 字节时 0x80 之后放不下长度字段，要多补一块——长度恰好卡在 55/56 的
	// 输入最容易在这里写错，测试里专门盯着这几个边界。
	uint8 Tail[128] = {};
	const uint64 Remainder = Length % 64;
	if (Remainder > 0)
	{
		FMemory::Memcpy(Tail, Bytes + FullBlocks * 64, static_cast<SIZE_T>(Remainder));
	}
	Tail[Remainder] = 0x80;

	const int32 TailBlocks = (Remainder < 56) ? 1 : 2;
	const uint64 BitLength = Length * 8;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailBlocks * 64 - 1 - Index] = static_cast<uint8>(BitLength >> (8 * Index));
	}
	ProcessBlock(State, Tail);
	if (TailBlocks == 2)
	{
		ProcessBlock(State, Tail + 64);
	}

	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutDigest[Index * 4 + 0] = static_cast<uint8>(State[Index] >> 24);
		OutDigest[Index * 4 + 1] = static_cast<uint8>(State[Index] >> 16);
		OutDigest[Index * 4 + 2] = static_cast<uint8>(State[Index] >> 8);
		OutDigest[Index * 4 + 3] = static_cast<uint8>(State[Index]);
	}
}
