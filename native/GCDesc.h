#pragma once

struct WalkObjectContext
{
    WalkObjectContext(ICorProfilerInfo10 *corProfilerInfo, ClassID stringTypeHandle, std::unordered_map<ULONG, ObjectID> *hashToObjectIDMap, ULONG stringLengthOffset, ULONG stringBufferOffset) : CorProfilerInfo(corProfilerInfo), StringTypeHandle(stringTypeHandle), HashToObjectIDMap(hashToObjectIDMap), StringLengthOffset(stringLengthOffset), StringBufferOffset(stringBufferOffset)
    {
    }

    ICorProfilerInfo10 *CorProfilerInfo;
    ClassID StringTypeHandle;
    std::unordered_map<ULONG, ObjectID> *HashToObjectIDMap;
    ULONG StringLengthOffset;
    ULONG StringBufferOffset;
};

typedef HRESULT (*WalkObjectFunc)(WalkObjectContext *, ObjectID, int32_t);

static int ComputeSize(int series)
{
    return sizeof(SIZE_T) + series * sizeof(SIZE_T) * 2;
}

class GCDesc
{
  private:
    uint8_t *data;
    size_t size;

    int32_t GetNumSeries()
    {
#ifdef _WIN64
        return (int32_t)(*(int64_t *)(this->data + this->size - sizeof(SIZE_T)));
#else
        return (int32_t)(*(int32_t *)(this->data + this->size - sizeof(SIZE_T)));
#endif
    }

    int32_t GetHighestSeries()
    {
        return (int32_t)(this->size - sizeof(SIZE_T) * 3);
    }

    int32_t GetLowestSeries()
    {
        return (int32_t)(this->size - ComputeSize(this->GetNumSeries()));
    }

    int32_t GetSeriesSize(int curr)
    {
#ifdef _WIN64
        return (int32_t)(*(int64_t *)(this->data + curr));
#else
        return (int32_t)(*(int32_t *)(this->data + curr));
#endif
    }

    uint64_t GetSeriesOffset(int curr)
    {
#ifdef _WIN64
        return (uint64_t)(*(uint64_t *)(this->data + curr + sizeof(SIZE_T)));
#else
        return (uint64_t)(*(uint32_t *)(this->data + curr + sizeof(SIZE_T)));
#endif
    }

    uint32_t GetPointers(int curr, int i)
    {
        int32_t offset = i * sizeof(SIZE_T);
#ifdef _WIN64
        return (uint32_t) * (uint32_t *)(this->data + curr + offset);
#else
        return (uint32_t) * (uint16_t *)(this->data + curr + offset);
#endif
    }

    uint32_t GetSkip(int curr, int i)
    {
        int32_t offset = i * sizeof(SIZE_T) + sizeof(SIZE_T) / 2;
#ifdef _WIN64
        return (uint32_t) * (uint32_t *)(this->data + curr + offset);
#else
        return (uint32_t) * (uint16_t *)(this->data + curr + offset);
#endif
    }

  public:
    GCDesc(uint8_t *data, size_t size) : data(data), size(size)
    {
    }

    void WalkObject(PBYTE addr, SIZE_T size, WalkObjectContext *context, WalkObjectFunc refCallback)
    {
        printf("Walk\n");

        int32_t series = this->GetNumSeries();
        int32_t highest = this->GetHighestSeries();
        int32_t curr = highest;

        if (series > 0)
        {
            int32_t lowest = this->GetLowestSeries();
            do
            {
                auto ptr = addr + this->GetSeriesOffset(curr);
                auto stop = ptr + GetSeriesSize(curr) + size;

                while (ptr < stop)
                {
                    auto ret = *(SIZE_T *)ptr;
                    if (ret != 0)
                    {
                        refCallback(context, (ObjectID)addr, (int32_t)(ptr - addr));
                    }

                    ptr += sizeof(SIZE_T);
                }

                curr -= sizeof(SIZE_T) * 2;
            } while (curr >= lowest);
        }
        else
        {
            auto ptr = addr + this->GetSeriesOffset(curr);
            while (ptr < addr + size - sizeof(SIZE_T))
            {
                for (int32_t i = 0; i > series; i--)
                {
                    uint32_t nptrs = this->GetPointers(curr, i);
                    uint32_t skip = this->GetSkip(curr, i);

                    auto stop = ptr + (nptrs * sizeof(SIZE_T));
                    do
                    {
                        auto ret = *(SIZE_T *)ptr;
                        if (ret != 0)
                        {
                            refCallback(context, (ObjectID)addr, (int32_t)(ptr - addr));
                        }

                        ptr += sizeof(SIZE_T);
                    } while (ptr < stop);

                    ptr += skip;
                }
            }
        }
    }
};