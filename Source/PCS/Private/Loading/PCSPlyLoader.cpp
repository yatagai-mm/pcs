#include "Loading/PCSPlyLoader.h"

#include "HAL/PlatformFileManager.h"
#include "Math/UnrealMathUtility.h"

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

/*
 * Parse PLY header from HeaderBytes until DataOffset.
 * If successful, OutHeader is populated and the function returns true.
 * Otherwise, OutError contains a description of the failure and the function returns false.
 *
 * This parser only reads the format, element and property lines
 */
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
		// Ignore other lines like comments
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

	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(ReadScalar(VertexData + Property->Offset, Property->Type)), 0, 255));
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

	for (int64 FirstVertex = 0; FirstVertex < Header.VertexCount; FirstVertex += VerticesPerChunk)
	{
		const int32 ChunkVertexCount = static_cast<int32>(FMath::Min<int64>(VerticesPerChunk, Header.VertexCount - FirstVertex));
		const int64 ChunkByteCount = static_cast<int64>(ChunkVertexCount) * Header.VertexStride;
		if (!FileHandle->Read(ReadBuffer.GetData(), ChunkByteCount))
		{
			return MakeLoadError(FString::Printf(TEXT("Unable to read PLY vertex payload at vertex %lld: %s"), FirstVertex, *FilePath));
		}

		for (int32 ChunkIndex = 0; ChunkIndex < ChunkVertexCount; ++ChunkIndex)
		{
			const uint8 *VertexData = ReadBuffer.GetData() + ChunkIndex * Header.VertexStride;
			FPCSPointVertex &Vertex = MutableFrame->Vertices[static_cast<int32>(FirstVertex) + ChunkIndex];
			Vertex.Position = FVector3f(static_cast<float>(ReadScalar(VertexData + XProperty->Offset, XProperty->Type)),
										static_cast<float>(ReadScalar(VertexData + YProperty->Offset, YProperty->Type)),
										static_cast<float>(ReadScalar(VertexData + ZProperty->Offset, ZProperty->Type)));

			if (!FMath::IsFinite(Vertex.Position.X) || !FMath::IsFinite(Vertex.Position.Y) || !FMath::IsFinite(Vertex.Position.Z))
			{
				return MakeLoadError(FString::Printf(TEXT("PLY vertex %lld contains a non-finite position: %s"), FirstVertex + ChunkIndex, *FilePath));
			}

			Vertex.Color = FColor(ReadColor(VertexData, RedProperty, 255), ReadColor(VertexData, GreenProperty, 255), ReadColor(VertexData, BlueProperty, 255),
								  ReadColor(VertexData, AlphaProperty, 255));
			MutableFrame->Bounds += Vertex.Position;
		}
	}

	FPCSPlyLoadResult Result;
	Result.FrameData = MutableFrame;
	return Result;
}
