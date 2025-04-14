#pragma once

#include <vector>


template<typename FnPtrT, typename UserDataT = void*>
struct GenericCallbackList
{
    struct Record { FnPtrT fn; UserDataT data; };

    void add(const FnPtrT& fn, const UserDataT& data)
    {
        for (const auto &rec : list)
            if (rec.fn == fn && rec.data == data)
                return;
        list.push_back({ fn, data });
    }

    void remove(const FnPtrT& fn, const UserDataT& data)
    {
        for (auto it = list.begin(); it != list.end(); it++)
            if (it->fn == fn && it->data == data)
            {
                list.erase(it);
                return;
            }
        list.push_back({ std::move(fn), std::move(data) });
    }

    template<typename... Args>
    void invokeAll(Args&& ...args)
    {
        for (const auto &rec : list)
            rec.fn(rec.data, std::forward<Args>(args)...);
    }

private:
    std::vector<Record> list;
};
