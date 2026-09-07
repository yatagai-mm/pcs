#include "Loading/PCSPlyLoader.h"

#include "HAL/PlatformFileManager.h"
#include "Math/UnrealMathUtility.h"
#include "Math/VectorRegister.h"

namespace
{
// This encoder stops parsing the header after 10KiB
constexpr int32 MaxSupportedHeaderSize = 10 * 1024;
constexpr int32 TargetReadSize = 4 * 1024 * 1024;

enum class EPCSPlyScalarType : uint8
{
	Int8,
	UInt8,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Float32,
	Float64
};

struct FPCSPlyProperty
{
	FString Name;
	EPCSPlyScalarType Type = EPCSPlyScalarType::UInt8;
	int32 Offset = 0;
	int32 Size = 0;
};

struct FPCSPlyHeader
{
	// Start byte of the payload
	int64 DataOffset = 0;
	// Number of vertices in the file
	int64 VertexCount = INDEX_NONE;
	// Length of the bytes for one vertex
	int32 VertexStride = 0;
	TArray<FPCSPlyProperty> VertexProperties;
	// Optional 8i frame-space metadata. Identity defaults preserve ordinary PLY
	// files that do not declare either comment.
	float FrameToWorldScale = 1.0f;
	FVector3f FrameToWorldTranslation = FVector3f::ZeroVector;
	bool bHasFrameToWorldTransform = false;
};

FPCSPlyLoadResult MakeLoadError(FString Message)
{
	FPCSPlyLoadResult Result;
	Result.ErrorMessage = MoveTemp(Message);
	return Result;
}

bool TryGetScalarType(const FString &TypeName, EPCSPlyScalarType &OutType, int32 &OutSize)
{
	if (TypeName == TEXT("char") || TypeName == TEXT("int8"))
	{
		OutType = EPCSPlyScalarType::Int8;
		OutSize = 1;
	}
	else if (TypeName == TEXT("uchar") || TypeName == TEXT("uint8"))
	{
		OutType = EPCSPlyScalarType::UInt8;
		OutSize = 1;
	}
	else if (TypeName == TEXT("short") || TypeName == TEXT("int16"))
	{
		OutType = EPCSPlyScalarType::Int16;
		OutSize = 2;
	}
	else if (TypeName == TEXT("ushort") || TypeName == TEXT("uint16"))
	{
		OutType = EPCSPlyScalarType::UInt16;
		OutSize = 2;
	}
	else if (TypeName == TEXT("int") || TypeName == TEXT("int32"))
	{
		OutType = EPCSPlyScalarType::Int32;
		OutSize = 4;
	}
	else if (TypeName == TEXT("uint") || TypeName == TEXT("uint32"))
	{
		OutType = EPCSPlyScalarType::UInt32;
		OutSize = 4;
	}
	else if (TypeName == TEXT("float") || TypeName == TEXT("float32"))
	{
		OutType = EPCSPlyScalarType::Float32;
		OutSize = 4;
	}
	else if (TypeName == TEXT("double") || TypeName == TEXT("float64"))
	{
		OutType = EPCSPlyScalarType::Float64;
		OutSize = 8;
	}
	else
	{
		return false;
	}

	return true;
}

uint16 ReadUInt16LittleEndian(const uint8 *Data) { return static_cast<uint16>(Data[0]) | static_cast<uint16>(Data[1]) << 8; }

uint32 ReadUInt32LittleEndian(const uint8 *Data)
{
	return static_cast<uint32>(Data[0]) | static_cast<uint32>(Data[1]) << 8 | static_cast<uint32>(Data[2]) << 16 | static_cast<uint32>(Data[3]) << 24;
}

uint64 ReadUInt64LittleEndian(const uint8 *Data)
{
	return static_cast<uint64>(ReadUInt32LittleEndian(Data)) | static_cast<uint64>(ReadUInt32LittleEndian(Data + 4)) << 32;
}

double ReadScalar(const uint8 *Data, EPCSPlyScalarType Type)
{
	switch (Type)
	{
	case EPCSPlyScalarType::Int8:
		return static_cast<int8>(Data[0]);
	case EPCSPlyScalarType::UInt8:
		return Data[0];
	case EPCSPlyScalarType::Int16:
		return static_cast<int16>(ReadUInt16LittleEndian(Data));
	case EPCSPlyScalarType::UInt16:
		return ReadUInt16LittleEndian(Data);
	case EPCSPlyScalarType::Int32:
		return static_cast<int32>(ReadUInt32LittleEndian(Data));
	case EPCSPlyScalarType::UInt32:
		return ReadUInt32LittleEndian(Data);
	case EPCSPlyScalarType::Float32:
	{
		const uint32 Bits = ReadUInt32LittleEndian(Data);
		float Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}
	case EPCSPlyScalarType::Float64:
	{
		const uint64 Bits = ReadUInt64LittleEndian(Data);
		double Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}
	default:
		checkNoEntry();
		return 0.0;
	}
}

int32 FindHeaderDataOffset(const TArray<uint8> &Bytes)
{
	constexpr ANSICHAR EndHeader[] = "end_header";
	constexpr int32 EndHeaderLength = UE_ARRAY_COUNT(EndHeader) - 1;

	for (int32 Index = 0; Index + EndHeaderLength <= Bytes.Num(); ++Index)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Index, EndHeader, EndHeaderLength) != 0)
		{
			continue;
		}

		const int32 AfterToken = Index + EndHeaderLength;
		if (AfterToken < Bytes.Num() && Bytes[AfterToken] == '\n')
		{
			return AfterToken + 1;
		}

		if (AfterToken + 1 < Bytes.Num() && Bytes[AfterToken] == '\r' && Bytes[AfterToken + 1] == '\n')
		{
			return AfterToken + 2;
		}
	}

	return INDEX_NONE;
}

// Parse PLY header from HeaderBytes until DataOffset.
// If successful, OutHeader is populated and the function returns true.
// Otherwise, OutError contains a description of the failure and the function returns false.
//
// In addition to the standard format, element and property lines, this parser
// recognizes 8i's frame_to_world_scale and frame_to_world_translation comments. (see https://plenodb.jpeg.org/pc/8ilabs)
// We support these non-standard comments only because 8i is a quite popular PLY dataset.
bool ParseHeader(const TArray<uint8> &HeaderBytes, int32 DataOffset, FPCSPlyHeader &OutHeader, FString &OutError)
{
	FString HeaderText;
	HeaderText.Reserve(DataOffset);
	for (int32 Index = 0; Index < DataOffset; ++Index)
	{
		HeaderText.AppendChar(static_cast<TCHAR>(HeaderBytes[Index]));
	}

	TArray<FString> Lines;
	HeaderText.ParseIntoArrayLines(Lines, false);
	if (Lines.IsEmpty() || Lines[0].TrimStartAndEnd() != TEXT("ply"))
	{
		// File must start with "ply"
		OutError = TEXT("The file does not start with the PLY magic header.");
		return false;
	}

	bool bHasSupportedFormat = false;
	bool bFoundVertexElement = false;
	bool bFoundEarlierElement = false;
	bool bReadingVertexProperties = false;

	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		TArray<FString> Tokens;
		Line.ParseIntoArrayWS(Tokens);
		if (Tokens.IsEmpty())
		{
			continue;
		}

		if (Tokens[0] == TEXT("format"))
		{
			bHasSupportedFormat = Tokens.Num() == 3 && Tokens[1] == TEXT("binary_little_endian") && Tokens[2] == TEXT("1.0");
		}
		else if (Tokens[0] == TEXT("comment") && Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("frame_to_world_scale"), ESearchCase::IgnoreCase))
		{
			if (Tokens.Num() != 3 || !LexTryParseString(OutHeader.FrameToWorldScale, *Tokens[2]) || !FMath::IsFinite(OutHeader.FrameToWorldScale))
			{
				OutError = FString::Printf(TEXT("Malformed frame_to_world_scale comment: %s"), *Line);
				return false;
			}
			OutHeader.bHasFrameToWorldTransform = true;
		}
		else if (Tokens[0] == TEXT("comment") && Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("frame_to_world_translation"), ESearchCase::IgnoreCase))
		{
			if (Tokens.Num() != 5 || !LexTryParseString(OutHeader.FrameToWorldTranslation.X, *Tokens[2]) ||
				!LexTryParseString(OutHeader.FrameToWorldTranslation.Y, *Tokens[3]) || !LexTryParseString(OutHeader.FrameToWorldTranslation.Z, *Tokens[4]) ||
				!FMath::IsFinite(OutHeader.FrameToWorldTranslation.X) || !FMath::IsFinite(OutHeader.FrameToWorldTranslation.Y) ||
				!FMath::IsFinite(OutHeader.FrameToWorldTranslation.Z))
			{
				OutError = FString::Printf(TEXT("Malformed frame_to_world_translation comment: %s"), *Line);
				return false;
			}
			OutHeader.bHasFrameToWorldTransform = true;
		}
		else if (Tokens[0] == TEXT("element"))
		{
			if (Tokens.Num() != 3)
			{
				OutError = FString::Printf(TEXT("Malformed element declaration: %s"), *Line);
				return false;
			}

			bReadingVertexProperties = Tokens[1] == TEXT("vertex");
			if (bReadingVertexProperties)
			{
				if (bFoundEarlierElement)
				{
					OutError = TEXT("The vertex element must be the first data element.");
					return false;
				}

				if (!LexTryParseString(OutHeader.VertexCount, *Tokens[2]) || OutHeader.VertexCount < 0)
				{
					OutError = FString::Printf(TEXT("Invalid vertex count: %s"), *Tokens[2]);
					return false;
				}
				bFoundVertexElement = true;
			}
			else if (!bFoundVertexElement)
			{
				bFoundEarlierElement = true;
			}
		}
		else if (Tokens[0] == TEXT("property") && bReadingVertexProperties)
		{
			if (Tokens.Num() != 3 || Tokens[1] == TEXT("list"))
			{
				OutError = FString::Printf(TEXT("Unsupported vertex property: %s"), *Line);
				return false;
			}

			FPCSPlyProperty Property;
			Property.Name = Tokens[2].ToLower();
			Property.Offset = OutHeader.VertexStride;
			if (!TryGetScalarType(Tokens[1].ToLower(), Property.Type, Property.Size))
			{
				OutError = FString::Printf(TEXT("Unsupported PLY scalar type: %s"), *Tokens[1]);
				return false;
			}

			OutHeader.VertexStride += Property.Size;
			OutHeader.VertexProperties.Add(MoveTemp(Property));
		}
		// Ignore unrecognized comments and other header lines.
	}

	if (!bHasSupportedFormat)
	{
		OutError = TEXT("Only binary_little_endian PLY 1.0 files are supported.");
		return false;
	}

	if (!bFoundVertexElement || OutHeader.VertexStride <= 0)
	{
		OutError = TEXT("The PLY header has no valid vertex element.");
		return false;
	}

	for (const TCHAR *RequiredName : {TEXT("x"), TEXT("y"), TEXT("z")})
	{
		if (!OutHeader.VertexProperties.ContainsByPredicate([RequiredName](const FPCSPlyProperty &Property) { return Property.Name == RequiredName; }))
		{
			OutError = FString::Printf(TEXT("The vertex element is missing the '%s' property."), RequiredName);
			return false;
		}
	}

	OutHeader.DataOffset = DataOffset;
	return true;
}

const FPCSPlyProperty *FindProperty(const FPCSPlyHeader &Header, const TCHAR *Name)
{
	return Header.VertexProperties.FindByPredicate([Name](const FPCSPlyProperty &Property) { return Property.Name == Name; });
}

uint8 ReadColor(const uint8 *VertexData, const FPCSPlyProperty *Property, uint8 DefaultValue)
{
	if (Property == nullptr)
	{
		return DefaultValue;
	}
	if (Property->Type == EPCSPlyScalarType::UInt8)
	{
		return VertexData[Property->Offset];
	}

	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(ReadScalar(VertexData + Property->Offset, Property->Type)), 0, 255));
}

bool HasPackedLayout(const FPCSPlyHeader &Header)
{
	if (Header.VertexStride != 15 && Header.VertexStride != 16)
	{
		return false;
	}

	const FPCSPlyProperty *XProperty = FindProperty(Header, TEXT("x"));
	const FPCSPlyProperty *YProperty = FindProperty(Header, TEXT("y"));
	const FPCSPlyProperty *ZProperty = FindProperty(Header, TEXT("z"));
	const FPCSPlyProperty *RedProperty = FindProperty(Header, TEXT("red"));
	const FPCSPlyProperty *GreenProperty = FindProperty(Header, TEXT("green"));
	const FPCSPlyProperty *BlueProperty = FindProperty(Header, TEXT("blue"));
	const FPCSPlyProperty *AlphaProperty = FindProperty(Header, TEXT("alpha"));
	return XProperty && YProperty && ZProperty && RedProperty && GreenProperty && BlueProperty && XProperty->Type == EPCSPlyScalarType::Float32 &&
		   YProperty->Type == EPCSPlyScalarType::Float32 && ZProperty->Type == EPCSPlyScalarType::Float32 && XProperty->Offset == 0 && YProperty->Offset == 4 &&
		   ZProperty->Offset == 8 && RedProperty->Type == EPCSPlyScalarType::UInt8 && GreenProperty->Type == EPCSPlyScalarType::UInt8 &&
		   BlueProperty->Type == EPCSPlyScalarType::UInt8 && RedProperty->Offset == 12 && GreenProperty->Offset == 13 && BlueProperty->Offset == 14 &&
		   (Header.VertexStride == 15 || (AlphaProperty && AlphaProperty->Type == EPCSPlyScalarType::UInt8 && AlphaProperty->Offset == 15));
}

bool DecodePackedVertex(const uint8 *VertexData, int32 VertexStride, FPCSPointVertex &Vertex)
{
	FMemory::Memcpy(&Vertex.Position, VertexData, sizeof(Vertex.Position));
	Vertex.Color = FColor(VertexData[12], VertexData[13], VertexData[14], VertexStride == 16 ? VertexData[15] : 255);
	return FMath::IsFinite(Vertex.Position.X) && FMath::IsFinite(Vertex.Position.Y) && FMath::IsFinite(Vertex.Position.Z);
}

template <int32 Stride, bool bApplyFrameToWorldTransform>
int32 DecodePackedSIMD(const uint8 *RESTRICT Source, FPCSPointVertex *RESTRICT Dest, int32 Count, FBox3f &Bounds, float FrameToWorldScale,
					   const FVector3f &FrameToWorldTranslation)
{
	const VectorRegister4Float ExponentMask = MakeVectorRegisterFloat(0x7f800000u, 0x7f800000u, 0x7f800000u, 0u);
	const VectorRegister4Float Scale = VectorSetFloat1(FrameToWorldScale);
	const VectorRegister4Float Translation = MakeVectorRegisterFloat(FrameToWorldTranslation.X, FrameToWorldTranslation.Y, FrameToWorldTranslation.Z, 0.0f);
	VectorRegister4Float Min[4], Max[4], Exponents[4];
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		Min[Lane] = MakeVectorRegisterFloat(MAX_flt, MAX_flt, MAX_flt, 0.0f);
		Max[Lane] = MakeVectorRegisterFloat(-MAX_flt, -MAX_flt, -MAX_flt, 0.0f);
		Exponents[Lane] = VectorZeroFloat();
	}

	// Leave a record for the scalar tail so a 16-byte load never reads beyond a 15-byte payload.
	const int32 VectorCount = Count > 0 ? ((Count - 1) / 4) * 4 : 0;
	for (int32 Index = 0; Index < VectorCount; Index += 4)
	{
		for (int32 Lane = 0; Lane < 4; ++Lane)
		{
			const uint8 *Record = Source + (Index + Lane) * Stride;
			VectorRegister4Float Raw;
			FMemory::Memcpy(&Raw, Record, 16);
			VectorRegister4Float Position = VectorSet_W0(Raw);
			if constexpr (bApplyFrameToWorldTransform)
			{
				Position = VectorMultiplyAdd(Position, Scale, Translation);
				VectorStoreFloat3(Position, &Dest[Index + Lane].Position.X);
			}
			else
			{
				FMemory::Memcpy(&Dest[Index + Lane], &Raw, 16);
			}
			Dest[Index + Lane].Color = FColor(Record[12], Record[13], Record[14], Stride == 16 ? Record[15] : 255);
			Min[Lane] = VectorMin(Min[Lane], Position);
			Max[Lane] = VectorMax(Max[Lane], Position);
			Exponents[Lane] = VectorMax(Exponents[Lane], VectorBitwiseAnd(Position, ExponentMask));
		}
	}

	const VectorRegister4Float AllExponents = VectorMax(VectorMax(Exponents[0], Exponents[1]), VectorMax(Exponents[2], Exponents[3]));
	if (VectorContainsNaNOrInfinite(AllExponents))
	{
		for (int32 Index = 0; Index < VectorCount; ++Index)
		{
			const FVector3f &Position = Dest[Index].Position;
			if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
			{
				return Index;
			}
		}
	}

	if (VectorCount > 0)
	{
		FVector3f ChunkMin, ChunkMax;
		VectorStoreFloat3(VectorMin(VectorMin(Min[0], Min[1]), VectorMin(Min[2], Min[3])), &ChunkMin.X);
		VectorStoreFloat3(VectorMax(VectorMax(Max[0], Max[1]), VectorMax(Max[2], Max[3])), &ChunkMax.X);
		Bounds += FBox3f(ChunkMin, ChunkMax);
	}

	for (int32 Index = VectorCount; Index < Count; ++Index)
	{
		if (!DecodePackedVertex(Source + Index * Stride, Stride, Dest[Index]))
		{
			return Index;
		}
		if constexpr (bApplyFrameToWorldTransform)
		{
			Dest[Index].Position = Dest[Index].Position * FrameToWorldScale + FrameToWorldTranslation;
			if (!FMath::IsFinite(Dest[Index].Position.X) || !FMath::IsFinite(Dest[Index].Position.Y) || !FMath::IsFinite(Dest[Index].Position.Z))
			{
				return Index;
			}
		}
		Bounds += Dest[Index].Position;
	}
	return INDEX_NONE;
}
} // namespace

FPCSPlyLoadResult FPCSPlyLoader::LoadFromFile(const FString &FilePath)
{
	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));
	if (!FileHandle)
	{
		return MakeLoadError(FString::Printf(TEXT("Unable to open PLY file: %s"), *FilePath));
	}

	const int64 FileSize = FileHandle->Size();
	if (FileSize <= 0)
	{
		return MakeLoadError(FString::Printf(TEXT("PLY file is empty: %s"), *FilePath));
	}

	const int32 HeaderProbeSize = static_cast<int32>(FMath::Min<int64>(FileSize, MaxSupportedHeaderSize));
	TArray<uint8> HeaderBytes;
	HeaderBytes.SetNumUninitialized(HeaderProbeSize);
	if (!FileHandle->Read(HeaderBytes.GetData(), HeaderProbeSize))
	{
		return MakeLoadError(FString::Printf(TEXT("Unable to read PLY header: %s"), *FilePath));
	}

	const int32 DataOffset = FindHeaderDataOffset(HeaderBytes);
	if (DataOffset == INDEX_NONE)
	{
		return MakeLoadError(FString::Printf(TEXT("PLY header terminator was not found within %d bytes: %s"), MaxSupportedHeaderSize, *FilePath));
	}

	FPCSPlyHeader Header;
	FString HeaderError;
	if (!ParseHeader(HeaderBytes, DataOffset, Header, HeaderError))
	{
		return MakeLoadError(FString::Printf(TEXT("%s File: %s"), *HeaderError, *FilePath));
	}

	if (Header.VertexCount > MAX_int32)
	{
		return MakeLoadError(FString::Printf(TEXT("PLY contains too many vertices for a TArray: %lld"), Header.VertexCount));
	}

	if (Header.VertexCount > (MAX_int64 - Header.DataOffset) / Header.VertexStride)
	{
		return MakeLoadError(TEXT("PLY vertex payload size overflows int64."));
	}

	const int64 RequiredFileSize = Header.DataOffset + Header.VertexCount * Header.VertexStride;
	if (FileSize < RequiredFileSize)
	{
		return MakeLoadError(FString::Printf(TEXT("PLY vertex payload is truncated. Expected at least %lld bytes, found %lld."), RequiredFileSize, FileSize));
	}

	const FPCSPlyProperty *XProperty = FindProperty(Header, TEXT("x"));
	const FPCSPlyProperty *YProperty = FindProperty(Header, TEXT("y"));
	const FPCSPlyProperty *ZProperty = FindProperty(Header, TEXT("z"));
	const FPCSPlyProperty *RedProperty = FindProperty(Header, TEXT("red"));
	const FPCSPlyProperty *GreenProperty = FindProperty(Header, TEXT("green"));
	const FPCSPlyProperty *BlueProperty = FindProperty(Header, TEXT("blue"));
	const FPCSPlyProperty *AlphaProperty = FindProperty(Header, TEXT("alpha"));

	TSharedRef<FPCSFrameData> MutableFrame = MakeShared<FPCSFrameData>();
	MutableFrame->Vertices.SetNumUninitialized(static_cast<int32>(Header.VertexCount));

	if (!FileHandle->Seek(Header.DataOffset))
	{
		return MakeLoadError(FString::Printf(TEXT("Unable to seek to PLY vertex data: %s"), *FilePath));
	}

	const int32 VerticesPerChunk = FMath::Max(1, TargetReadSize / Header.VertexStride);
	TArray<uint8> ReadBuffer;
	ReadBuffer.SetNumUninitialized(VerticesPerChunk * Header.VertexStride);
	const bool bHasPackedLayout = HasPackedLayout(Header);
	using FPackedDecoder = int32 (*)(const uint8 *, FPCSPointVertex *, int32, FBox3f &, float, const FVector3f &);
	const FPackedDecoder PackedDecoder =
		bHasPackedLayout ? (Header.VertexStride == 15 ? (Header.bHasFrameToWorldTransform ? &DecodePackedSIMD<15, true> : &DecodePackedSIMD<15, false>)
													  : (Header.bHasFrameToWorldTransform ? &DecodePackedSIMD<16, true> : &DecodePackedSIMD<16, false>))
						 : nullptr;

	for (int64 FirstVertex = 0; FirstVertex < Header.VertexCount; FirstVertex += VerticesPerChunk)
	{
		const int32 ChunkVertexCount = static_cast<int32>(FMath::Min<int64>(VerticesPerChunk, Header.VertexCount - FirstVertex));
		const int64 ChunkByteCount = static_cast<int64>(ChunkVertexCount) * Header.VertexStride;
		if (!FileHandle->Read(ReadBuffer.GetData(), ChunkByteCount))
		{
			return MakeLoadError(FString::Printf(TEXT("Unable to read PLY vertex payload at vertex %lld: %s"), FirstVertex, *FilePath));
		}

		if (PackedDecoder)
		{
			FPCSPointVertex *Dest = MutableFrame->Vertices.GetData() + FirstVertex;
			const int32 InvalidIndex =
				PackedDecoder(ReadBuffer.GetData(), Dest, ChunkVertexCount, MutableFrame->Bounds, Header.FrameToWorldScale, Header.FrameToWorldTranslation);
			if (InvalidIndex != INDEX_NONE)
			{
				return MakeLoadError(FString::Printf(TEXT("PLY vertex %lld contains a non-finite position: %s"), FirstVertex + InvalidIndex, *FilePath));
			}
		}
		else
		{
			// Accumulate extrema locally and merge once per chunk, avoiding the
			// FBox validity check for every vertex on the generic decode path.
			FVector3f Min(MAX_flt, MAX_flt, MAX_flt);
			FVector3f Max(-MAX_flt, -MAX_flt, -MAX_flt);
			FPCSPointVertex *Dest = MutableFrame->Vertices.GetData() + FirstVertex;
			for (int32 ChunkIndex = 0; ChunkIndex < ChunkVertexCount; ++ChunkIndex)
			{
				const uint8 *VertexData = ReadBuffer.GetData() + ChunkIndex * Header.VertexStride;
				FPCSPointVertex &Vertex = Dest[ChunkIndex];
				Vertex.Position = FVector3f(static_cast<float>(ReadScalar(VertexData + XProperty->Offset, XProperty->Type)),
											static_cast<float>(ReadScalar(VertexData + YProperty->Offset, YProperty->Type)),
											static_cast<float>(ReadScalar(VertexData + ZProperty->Offset, ZProperty->Type)));
				if (Header.bHasFrameToWorldTransform)
				{
					Vertex.Position = Vertex.Position * Header.FrameToWorldScale + Header.FrameToWorldTranslation;
				}

				if (!FMath::IsFinite(Vertex.Position.X) || !FMath::IsFinite(Vertex.Position.Y) || !FMath::IsFinite(Vertex.Position.Z))
				{
					return MakeLoadError(FString::Printf(TEXT("PLY vertex %lld contains a non-finite position: %s"), FirstVertex + ChunkIndex, *FilePath));
				}

				Vertex.Color = FColor(ReadColor(VertexData, RedProperty, 255), ReadColor(VertexData, GreenProperty, 255),
									  ReadColor(VertexData, BlueProperty, 255), ReadColor(VertexData, AlphaProperty, 255));
				Min.X = FMath::Min(Min.X, Vertex.Position.X);
				Min.Y = FMath::Min(Min.Y, Vertex.Position.Y);
				Min.Z = FMath::Min(Min.Z, Vertex.Position.Z);
				Max.X = FMath::Max(Max.X, Vertex.Position.X);
				Max.Y = FMath::Max(Max.Y, Vertex.Position.Y);
				Max.Z = FMath::Max(Max.Z, Vertex.Position.Z);
			}
			MutableFrame->Bounds += FBox3f(Min, Max);
		}
	}

	FPCSPlyLoadResult Result;
	Result.FrameData = MutableFrame;
	return Result;
}
