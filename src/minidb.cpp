#include "minidb/minidb.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <cctype>
#include <type_traits>
#include <limits>
#include <functional>
#include <set>

namespace minidb {
DiskManager::DiskManager(const std::string &name) {
  bool exists=std::filesystem::exists(name);
  file_.open(name,std::ios::in|std::ios::out|std::ios::binary|(!exists?std::ios::trunc:std::ios::openmode(0)));
  if(!file_) throw std::runtime_error("cannot open database");
  file_.seekg(0,std::ios::end);
  const auto end=file_.tellg();
  if(end<0||end%static_cast<std::streamoff>(PAGE_SIZE)!=0) throw std::runtime_error("database file is not page-aligned");
  const auto pages=end/static_cast<std::streamoff>(PAGE_SIZE);
  if(pages>std::numeric_limits<page_id_t>::max()) throw std::runtime_error("database is too large");
  next_=static_cast<page_id_t>(pages);
}
DiskManager::~DiskManager(){ Flush(); file_.close(); }
page_id_t DiskManager::AllocatePage(){ if(next_==std::numeric_limits<page_id_t>::max()) throw std::overflow_error("page id exhausted"); return next_++; }
bool DiskManager::ReadPage(page_id_t id,char *out){ if(id<0||out==nullptr||id>=next_)return false; file_.clear(); file_.seekg(static_cast<std::streamoff>(id)*PAGE_SIZE); if(!file_)return false; file_.read(out,static_cast<std::streamsize>(PAGE_SIZE)); if(file_.gcount()!=static_cast<std::streamsize>(PAGE_SIZE)){file_.clear(); return false;} return true; }
bool DiskManager::WritePage(page_id_t id,const char *data){ if(id<0||data==nullptr||id>=next_)return false; file_.clear(); file_.seekp(static_cast<std::streamoff>(id)*PAGE_SIZE); if(!file_)return false; file_.write(data,static_cast<std::streamsize>(PAGE_SIZE)); return static_cast<bool>(file_); }
void DiskManager::Flush(){ file_.flush(); if(!file_) throw std::runtime_error("database flush failed"); }

BufferPoolManager::BufferPoolManager(DiskManager &d, LogManager *wal):disk_(d),wal_(wal){ for(frame_id_t i=0;i<static_cast<frame_id_t>(POOL_SIZE);++i){pages_[i].id=INVALID_PAGE_ID;evictable_[i]=true;lru_.push_back(i);positions_[i]=std::prev(lru_.end());} }
void BufferPoolManager::RemoveFromLru(frame_id_t f){if(!evictable_[f])return;lru_.erase(positions_[f]);evictable_[f]=false;}
void BufferPoolManager::AddToLru(frame_id_t f){if(evictable_[f])return;lru_.push_front(f);positions_[f]=lru_.begin();evictable_[f]=true;}
void BufferPoolManager::Touch(frame_id_t f){if(!evictable_[f])return;lru_.erase(positions_[f]);lru_.push_front(f);positions_[f]=lru_.begin();}
frame_id_t BufferPoolManager::Victim(){return lru_.empty()?-1:lru_.back();}
void BufferPoolManager::FlushWalBeforeData(){if(wal_)wal_->Flush();}
Page *BufferPoolManager::FetchPage(page_id_t id){ if(id<0||id>=disk_.PageCount())return nullptr; auto it=page_table_.find(id); if(it!=page_table_.end()){RemoveFromLru(it->second);++pages_[it->second].pin_count;return &pages_[it->second];} frame_id_t f=Victim(); if(f<0)return nullptr; RemoveFromLru(f); Page &p=pages_[f]; if(p.id!=INVALID_PAGE_ID&&p.dirty){FlushWalBeforeData();if(!disk_.WritePage(p.id,p.data.data())){AddToLru(f);return nullptr;}} if(p.id!=INVALID_PAGE_ID)page_table_.erase(p.id); p={};p.id=id;p.pin_count=1;if(!disk_.ReadPage(id,p.data.data())){p={};AddToLru(f);return nullptr;}page_table_[id]=f;return &p; }
Page *BufferPoolManager::NewPage(page_id_t *id){ if(id==nullptr)return nullptr; frame_id_t f=Victim();if(f<0)return nullptr;RemoveFromLru(f);Page&p=pages_[f];if(p.id!=INVALID_PAGE_ID&&p.dirty){FlushWalBeforeData();if(!disk_.WritePage(p.id,p.data.data())){AddToLru(f);return nullptr;}}if(p.id!=INVALID_PAGE_ID)page_table_.erase(p.id);*id=disk_.AllocatePage();p={};p.id=*id;p.pin_count=1;page_table_[*id]=f;return&p; }
bool BufferPoolManager::UnpinPage(page_id_t id,bool dirty){auto it=page_table_.find(id);if(it==page_table_.end())return false;Page&p=pages_[it->second];if(p.pin_count==0)return false;--p.pin_count;p.dirty|=dirty;if(p.pin_count==0)AddToLru(it->second);return true;}
bool BufferPoolManager::DeletePage(page_id_t id){auto it=page_table_.find(id);if(it==page_table_.end())return true; if(pages_[it->second].pin_count)return false;RemoveFromLru(it->second);pages_[it->second]={};page_table_.erase(it);return true;}
void BufferPoolManager::FlushAllPages(){FlushWalBeforeData();for(auto&[id,f]:page_table_)if(pages_[f].dirty){if(!disk_.WritePage(id,pages_[f].data.data()))throw std::runtime_error("page flush failed");pages_[f].dirty=false;}disk_.Flush();}

int Schema::IndexOf(std::string_view n) const{for(std::size_t i=0;i<columns_.size();++i)if(columns_[i].name==n)return static_cast<int>(i);return -1;}
std::string Tuple::ToString() const{std::string s;for(std::size_t i=0;i<values_.size();++i){if(i)s+=',';std::visit([&](auto const&v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::string>)s+=v;else if constexpr(std::is_same_v<T,bool>)s+=v?"TRUE":"FALSE";else s+=std::to_string(v);},values_[i]);}return s;}
static void Put(std::vector<char>& b,const void* p,std::size_t n){b.insert(b.end(),static_cast<const char*>(p),static_cast<const char*>(p)+n);}
template<class T> static void Put(std::vector<char>& b,const T& x){Put(b,&x,sizeof(x));}
template<class T> static T Take(const char*&p,const char* end){if(p>end||static_cast<std::size_t>(end-p)<sizeof(T))throw std::runtime_error("page deserialization overflow");T x;std::memcpy(&x,p,sizeof(x));p+=sizeof(x);return x;}
TableHeap::TableHeap(BufferPoolManager&b,page_id_t first):bpm_(b),page_(first){if(first!=INVALID_PAGE_ID)Load();}
void TableHeap::Load(){page_id_t p=page_;while(p!=INVALID_PAGE_ID){Page*x=bpm_.FetchPage(p);if(!x)break;const char*q=x->data.data(),*end=q+PAGE_SIZE;try{std::uint32_t magic=Take<std::uint32_t>(q,end),n=Take<std::uint32_t>(q,end);page_id_t next=Take<page_id_t>(q,end);if((magic!=0x54424c31&&magic!=0x54424c32)||n==0){bpm_.UnpinPage(p,false);break;}tuple_pages_.push_back(p);bool deleted=magic==0x54424c32&&Take<std::uint8_t>(q,end)!=0;std::uint32_t len=Take<std::uint32_t>(q,end);std::vector<Value> vals;for(std::uint32_t i=0;i<len;++i){auto type=Take<std::uint8_t>(q,end);auto sz=Take<std::uint32_t>(q,end);if(type==0)vals.emplace_back(Take<std::int32_t>(q,end));else if(type==2)vals.emplace_back(Take<std::uint8_t>(q,end)!=0);else{if(sz>static_cast<std::size_t>(end-q))throw std::runtime_error("tuple string exceeds page");vals.emplace_back(std::string(q,q+sz));q+=sz;}}tuples_.emplace_back(std::move(vals));deleted_.push_back(deleted);bpm_.UnpinPage(p,false);p=next;}catch(...){bpm_.UnpinPage(p,false);throw;}}}
void TableHeap::FlushPage(page_id_t id){std::size_t i=std::find(tuple_pages_.begin(),tuple_pages_.end(),id)-tuple_pages_.begin();if(i>=tuples_.size())return;Page*x=bpm_.FetchPage(id);if(!x)throw std::runtime_error("cannot fetch table page");std::vector<char>b;std::uint32_t magic=0x54424c32,n=1;page_id_t next=(i+1<tuple_pages_.size()?tuple_pages_[i+1]:INVALID_PAGE_ID);Put(b,magic);Put(b,n);Put(b,next);std::uint8_t deleted=deleted_[i]?1:0;Put(b,deleted);auto const&vals=tuples_[i].Values();std::uint32_t len=static_cast<std::uint32_t>(vals.size());Put(b,len);for(auto const&v:vals)std::visit([&](auto const&z){using T=std::decay_t<decltype(z)>;std::uint8_t type=std::is_same_v<T,std::string>?1:std::is_same_v<T,bool>?2:0;std::uint32_t sz=4;if constexpr(std::is_same_v<T,std::string>)sz=static_cast<std::uint32_t>(z.size());else if constexpr(std::is_same_v<T,bool>)sz=1;Put(b,type);Put(b,sz);if constexpr(std::is_same_v<T,std::string>)b.insert(b.end(),z.begin(),z.end());else if constexpr(std::is_same_v<T,bool>){std::uint8_t value=z?1:0;Put(b,value);}else Put(b,z);},v);if(b.size()>PAGE_SIZE){bpm_.UnpinPage(id,false);throw std::length_error("tuple serialization exceeds page");}std::memset(x->data.data(),0,PAGE_SIZE);std::memcpy(x->data.data(),b.data(),b.size());bpm_.UnpinPage(id,true);}
RID TableHeap::Insert(const Tuple&t){page_id_t id;if(!bpm_.NewPage(&id))throw std::runtime_error("buffer pool full");std::vector<page_id_t> old_pages=tuple_pages_;tuple_pages_.push_back(id);tuples_.push_back(t);deleted_.push_back(false);try{if(page_==INVALID_PAGE_ID)page_=id;if(tuple_pages_.size()>1)FlushPage(tuple_pages_[tuple_pages_.size()-2]);FlushPage(id);}catch(...){tuple_pages_=std::move(old_pages);tuples_.pop_back();deleted_.pop_back();bpm_.UnpinPage(id,false);throw;}bpm_.UnpinPage(id,false);return {id,0};}
std::optional<Tuple> TableHeap::Get(const RID&r)const{auto it=std::find(tuple_pages_.begin(),tuple_pages_.end(),r.page_id);if(it==tuple_pages_.end()||r.slot!=0||deleted_[it-tuple_pages_.begin()])return{};return tuples_[it-tuple_pages_.begin()];}
bool TableHeap::Update(const RID&r,const Tuple&t){auto it=std::find(tuple_pages_.begin(),tuple_pages_.end(),r.page_id);if(it==tuple_pages_.end()||r.slot!=0||deleted_[it-tuple_pages_.begin()])return false;tuples_[it-tuple_pages_.begin()]=t;FlushPage(r.page_id);return true;}
bool TableHeap::Delete(const RID&r){auto it=std::find(tuple_pages_.begin(),tuple_pages_.end(),r.page_id);if(it==tuple_pages_.end()||r.slot!=0||deleted_[it-tuple_pages_.begin()])return false;deleted_[it-tuple_pages_.begin()]=true;FlushPage(r.page_id);return true;}
std::vector<std::pair<RID,Tuple>> TableHeap::Scan()const{std::vector<std::pair<RID,Tuple>> out;for(std::size_t i=0;i<tuples_.size();++i)if(!deleted_[i])out.push_back({{tuple_pages_[i],0},tuples_[i]});return out;}
SeqIterator::SeqIterator(const TableHeap&h):rows_(h.Scan()){} bool SeqIterator::HasNext()const{return pos_<rows_.size();} std::pair<RID,Tuple> SeqIterator::Next(){if(!HasNext())throw std::out_of_range("iterator");return rows_[pos_++];}

BPlusTree::BPlusTree(BufferPoolManager& b, page_id_t r) : bpm_(b), root_(r) {
  if (root_ == INVALID_PAGE_ID) {
    Node n{true, INVALID_PAGE_ID, INVALID_PAGE_ID, {}, {}, {}};
    root_ = NewNode(n);
  } else {
    Load();
  }
}

BPlusTree::Node BPlusTree::Read(page_id_t id) const {
  Page *page = bpm_.FetchPage(id);
  if (!page) throw std::runtime_error("cannot fetch index page");
  Node n;
  try {
    const char *q = page->data.data(), *end = q + PAGE_SIZE;
    const auto magic = Take<std::uint32_t>(q, end);
    const auto count = Take<std::uint32_t>(q, end);
    if (magic != 0x4c454146 && magic != 0x494e544e)
      throw std::runtime_error("invalid index page magic");
    n.leaf = magic == 0x4c454146;
    n.parent = Take<page_id_t>(q, end);
    n.next = Take<page_id_t>(q, end);
    const std::size_t capacity = n.leaf ? LeafCapacity() : InternalCapacity();
    if (count > capacity) throw std::runtime_error("index page capacity exceeded");
    for (std::uint32_t i = 0; i < count; ++i) n.keys.push_back(Take<std::int32_t>(q, end));
    if (n.leaf) {
      for (std::uint32_t i = 0; i < count; ++i) {
        RID rid{Take<page_id_t>(q, end), Take<std::uint16_t>(q, end)};
        n.values.push_back(rid);
      }
    } else {
      for (std::uint32_t i = 0; i <= count; ++i) n.children.push_back(Take<page_id_t>(q, end));
    }
  } catch (...) {
    bpm_.UnpinPage(id, false);
    throw;
  }
  bpm_.UnpinPage(id, false);
  return n;
}

void BPlusTree::Write(page_id_t id, const Node& n) const {
  const std::size_t capacity = n.leaf ? LeafCapacity() : InternalCapacity();
  if (n.keys.size() > capacity || (n.leaf ? n.values.size() != n.keys.size()
                                            : n.children.size() != n.keys.size() + 1))
    throw std::length_error("invalid index node");
  Page *page = bpm_.FetchPage(id);
  if (!page) throw std::runtime_error("cannot fetch index page for write");
  std::memset(page->data.data(), 0, PAGE_SIZE);
  char *q = page->data.data();
  const std::uint32_t magic = n.leaf ? 0x4c454146 : 0x494e544e;
  const std::uint32_t count = static_cast<std::uint32_t>(n.keys.size());
  std::memcpy(q, &magic, 4); q += 4; std::memcpy(q, &count, 4); q += 4;
  std::memcpy(q, &n.parent, 4); q += 4; std::memcpy(q, &n.next, 4); q += 4;
  for (auto key : n.keys) { std::memcpy(q, &key, 4); q += 4; }
  if (n.leaf) {
    for (auto rid : n.values) {
      std::memcpy(q, &rid.page_id, 4); q += 4;
      std::memcpy(q, &rid.slot, 2); q += 2;
    }
  } else {
    for (auto child : n.children) { std::memcpy(q, &child, 4); q += 4; }
  }
  bpm_.UnpinPage(id, true);
}

page_id_t BPlusTree::NewNode(const Node& n) {
  page_id_t id = INVALID_PAGE_ID;
  Page *page = bpm_.NewPage(&id);
  if (!page) throw std::runtime_error("buffer pool full while creating index page");
  bpm_.UnpinPage(id, false);
  Write(id, n);
  return id;
}

void BPlusTree::Load() {
  if (root_ < 0) throw std::runtime_error("invalid index root page");
  (void)Read(root_);
}

std::optional<std::pair<std::int32_t,page_id_t>>
BPlusTree::InsertRec(page_id_t id, std::int32_t key, RID rid) {
  Node n = Read(id);
  if (n.leaf) {
    auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
    if (it != n.keys.end() && *it == key) return std::nullopt;
    const auto pos = static_cast<std::size_t>(it - n.keys.begin());
    n.keys.insert(it, key);
    n.values.insert(n.values.begin() + static_cast<std::ptrdiff_t>(pos), rid);
    if (n.keys.size() <= LeafCapacity()) { Write(id, n); return std::nullopt; }
    const std::size_t mid = n.keys.size() / 2;
    Node right{true, n.parent, n.next,
               {n.keys.begin() + static_cast<std::ptrdiff_t>(mid), n.keys.end()},
               {n.values.begin() + static_cast<std::ptrdiff_t>(mid), n.values.end()}, {}};
    n.keys.resize(mid); n.values.resize(mid);
    const page_id_t right_id = NewNode(right);
    n.next = right_id;
    Write(id, n);
    return std::make_pair(right.keys.front(), right_id);
  }
  const std::size_t child_index = static_cast<std::size_t>(
      std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin());
  auto split = InsertRec(n.children.at(child_index), key, rid);
  if (!split) return std::nullopt;
  n.keys.insert(n.keys.begin() + static_cast<std::ptrdiff_t>(child_index), split->first);
  n.children.insert(n.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1), split->second);
  if (n.keys.size() <= InternalCapacity()) { Write(id, n); return std::nullopt; }
  const std::size_t mid = n.keys.size() / 2;
  const auto promoted = n.keys[mid];
  Node right{false, n.parent, INVALID_PAGE_ID,
             {n.keys.begin() + static_cast<std::ptrdiff_t>(mid + 1), n.keys.end()}, {},
             {n.children.begin() + static_cast<std::ptrdiff_t>(mid + 1), n.children.end()}};
  n.keys.resize(mid); n.children.resize(mid + 1);
  const page_id_t right_id = NewNode(right);
  for (auto child : right.children) {
    Node child_node = Read(child); child_node.parent = right_id; Write(child, child_node);
  }
  Write(id, n);
  return std::make_pair(promoted, right_id);
}

void BPlusTree::InsertParent(page_id_t, std::int32_t, page_id_t) {}

bool BPlusTree::Insert(std::int32_t key, RID rid) {
  if (Search(key).has_value()) return false;
  auto split = InsertRec(root_, key, rid);
  if (split) {
    Node root{false, INVALID_PAGE_ID, INVALID_PAGE_ID, {split->first}, {}, {root_, split->second}};
    const page_id_t old_root = root_;
    root_ = NewNode(root);
    Node left = Read(old_root); left.parent = root_; Write(old_root, left);
    Node right = Read(split->second); right.parent = root_; Write(split->second, right);
  }
  return true;
}

std::optional<RID> BPlusTree::Search(std::int32_t key) const {
  page_id_t id = root_;
  for (;;) {
    Node n = Read(id);
    if (n.leaf) {
      auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
      return it != n.keys.end() && *it == key
                 ? std::optional<RID>(n.values[static_cast<std::size_t>(it - n.keys.begin())])
                 : std::nullopt;
    }
    id = n.children.at(static_cast<std::size_t>(
        std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin()));
  }
}

std::vector<std::pair<std::int32_t,RID>>
BPlusTree::Range(std::int32_t low, std::int32_t high) const {
  std::vector<std::pair<std::int32_t,RID>> result;
  if (low > high) return result;
  page_id_t id = root_;
  for (;;) {
    Node n = Read(id);
    if (n.leaf) {
      for (std::size_t i = 0; i < n.keys.size(); ++i)
        if (n.keys[i] >= low && n.keys[i] <= high) result.push_back({n.keys[i], n.values[i]});
      if (n.next == INVALID_PAGE_ID) break;
      id = n.next;
    } else {
      id = n.children.at(static_cast<std::size_t>(
          std::upper_bound(n.keys.begin(), n.keys.end(), low) - n.keys.begin()));
    }
  }
  return result;
}

bool BPlusTree::Validate() const {
  try {
    std::vector<page_id_t> leaves;
    int leaf_depth = -1;
    std::function<bool(page_id_t,page_id_t,std::optional<std::int32_t>,std::optional<std::int32_t>,int)> visit =
      [&](page_id_t id, page_id_t parent, std::optional<std::int32_t> lo,
          std::optional<std::int32_t> hi, int depth) {
        Node n = Read(id);
        if (n.parent != parent || n.keys.size() > (n.leaf ? LeafCapacity() : InternalCapacity()))
          return false;
        if (depth == 0 && !n.leaf && n.keys.empty()) return false;
        if (!std::is_sorted(n.keys.begin(), n.keys.end())) return false;
        // Separators are the first key of the right child, so lower bounds
        // are inclusive while upper bounds are exclusive.
        for (auto k : n.keys) if ((lo && k < *lo) || (hi && k >= *hi)) return false;
        if (n.leaf) {
          if (n.values.size() != n.keys.size()) return false;
          if (leaf_depth < 0) leaf_depth = depth;
          if (leaf_depth != depth) return false;
          leaves.push_back(id); return true;
        }
        if (n.children.size() != n.keys.size() + 1) return false;
        for (std::size_t i = 0; i < n.children.size(); ++i) {
          auto child_lo = i == 0 ? lo : std::optional<std::int32_t>(n.keys[i - 1]);
          auto child_hi = i == n.keys.size() ? hi : std::optional<std::int32_t>(n.keys[i]);
          if (!visit(n.children[i], id, child_lo, child_hi, depth + 1)) return false;
        }
        return true;
      };
    if (!visit(root_, INVALID_PAGE_ID, {}, {}, 0)) return false;
    page_id_t id = leaves.empty() ? INVALID_PAGE_ID : leaves.front();
    std::size_t index = 0;
    std::optional<std::int32_t> previous;
    while (id != INVALID_PAGE_ID) {
      if (index >= leaves.size() || leaves[index++] != id) return false;
      Node n = Read(id);
      for (auto key : n.keys) { if (previous && key <= *previous) return false; previous = key; }
      id = n.next;
    }
    return index == leaves.size();
  } catch (...) { return false; }
}

static std::string EncodeTuple(const Tuple &tuple) {
  std::vector<char> bytes;
  const std::uint32_t count = static_cast<std::uint32_t>(tuple.Values().size());
  Put(bytes, count);
  for (const auto &value : tuple.Values()) {
    std::visit([&](const auto &item) {
      using T = std::decay_t<decltype(item)>;
      const std::uint8_t type = std::is_same_v<T, std::string> ? 1 :
                                std::is_same_v<T, bool> ? 2 : 0;
      Put(bytes, type);
      if constexpr (std::is_same_v<T, std::string>) {
        const auto length = static_cast<std::uint32_t>(item.size());
        Put(bytes, length);
        bytes.insert(bytes.end(), item.begin(), item.end());
      } else if constexpr (std::is_same_v<T, bool>) {
        const std::uint32_t length = 1;
        const std::uint8_t value = item ? 1 : 0;
        Put(bytes, length);
        Put(bytes, value);
      } else {
        const std::uint32_t length = sizeof(item);
        Put(bytes, length);
        Put(bytes, item);
      }
    }, value);
  }
  return std::string(bytes.begin(), bytes.end());
}

static Tuple DecodeTuple(const std::string &encoded) {
  const char *cursor = encoded.data();
  const char *end = cursor + encoded.size();
  const auto count = Take<std::uint32_t>(cursor, end);
  std::vector<Value> values;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto type = Take<std::uint8_t>(cursor, end);
    const auto length = Take<std::uint32_t>(cursor, end);
    if (type == 0 && length == sizeof(std::int32_t)) {
      values.emplace_back(Take<std::int32_t>(cursor, end));
    } else if (type == 2 && length == 1) {
      values.emplace_back(Take<std::uint8_t>(cursor, end) != 0);
    } else if (type == 1 && length <= static_cast<std::size_t>(end - cursor)) {
      values.emplace_back(std::string(cursor, cursor + length));
      cursor += length;
    } else {
      throw std::runtime_error("invalid WAL tuple");
    }
  }
  if (cursor != end) throw std::runtime_error("trailing WAL tuple data");
  return Tuple(std::move(values));
}

template <class T>
static void AppendBinary(std::ofstream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

LogManager::LogManager(const std::string &file) : file_(file) { Load(); }

void LogManager::Load() {
  records_.clear();
  std::ifstream in(file_, std::ios::binary);
  if (!in) return;
  constexpr std::uint32_t magic = 0x4d57414c;
  constexpr std::uint32_t header_size = 34;
  std::uint64_t valid_bytes = 0;
  while (true) {
    std::uint32_t record_magic = 0, length = 0;
    std::uint16_t version = 0;
    std::uint8_t type = 0, reserved = 0;
    std::uint64_t txn = 0;
    page_id_t page = INVALID_PAGE_ID;
    std::uint16_t slot = 0;
    std::uint32_t before_length = 0, after_length = 0;
    in.read(reinterpret_cast<char *>(&record_magic), sizeof(record_magic));
    if (in.eof()) break;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    in.read(reinterpret_cast<char *>(&type), sizeof(type));
    in.read(reinterpret_cast<char *>(&reserved), sizeof(reserved));
    in.read(reinterpret_cast<char *>(&length), sizeof(length));
    in.read(reinterpret_cast<char *>(&txn), sizeof(txn));
    in.read(reinterpret_cast<char *>(&page), sizeof(page));
    in.read(reinterpret_cast<char *>(&slot), sizeof(slot));
    in.read(reinterpret_cast<char *>(&before_length), sizeof(before_length));
    in.read(reinterpret_cast<char *>(&after_length), sizeof(after_length));
    if (!in || record_magic != magic || version != 1 || length < header_size ||
        length != header_size + before_length + after_length ||
        length > 64 * 1024 * 1024 ||
        (type > static_cast<std::uint8_t>(LogType::Abort))) break;
    LogRecord record{static_cast<LogType>(type), txn, {page, slot}, {}, {}};
    record.before.resize(before_length);
    record.after.resize(after_length);
    in.read(record.before.data(), before_length);
    in.read(record.after.data(), after_length);
    if (!in) break;
    records_.push_back(std::move(record));
    valid_bytes += length;
  }
  in.close();
  std::error_code error;
  const auto actual = std::filesystem::file_size(file_, error);
  if (!error && actual != valid_bytes) std::filesystem::resize_file(file_, valid_bytes, error);
}

void LogManager::Append(const LogRecord &record) {
  constexpr std::uint32_t magic = 0x4d57414c;
  constexpr std::uint16_t version = 1;
  constexpr std::uint32_t header_size = 34;
  const auto before_length = static_cast<std::uint32_t>(record.before.size());
  const auto after_length = static_cast<std::uint32_t>(record.after.size());
  const auto length = header_size + before_length + after_length;
  std::ofstream out(file_, std::ios::binary | std::ios::app);
  if (!out) throw std::runtime_error("cannot open WAL");
  AppendBinary(out, magic); AppendBinary(out, version);
  const auto type = static_cast<std::uint8_t>(record.type);
  const std::uint8_t reserved = 0;
  AppendBinary(out, type); AppendBinary(out, reserved); AppendBinary(out, length);
  AppendBinary(out, record.txn); AppendBinary(out, record.rid.page_id);
  AppendBinary(out, record.rid.slot); AppendBinary(out, before_length);
  AppendBinary(out, after_length);
  out.write(record.before.data(), record.before.size());
  out.write(record.after.data(), record.after.size());
  if (!out) throw std::runtime_error("WAL append failed");
  records_.push_back(record);
}
void LogManager::AppendBegin(std::uint64_t txn) { Append({LogType::Begin, txn, {}, {}, {}}); }
void LogManager::AppendInsert(std::uint64_t txn, RID rid, const Tuple &tuple) {
  Append({LogType::Insert, txn, rid, {}, EncodeTuple(tuple)});
}
void LogManager::AppendUpdate(std::uint64_t txn, RID rid, const Tuple &before,
                              const Tuple &after) {
  Append({LogType::Update, txn, rid, EncodeTuple(before), EncodeTuple(after)});
}
void LogManager::AppendCommit(std::uint64_t txn) { Append({LogType::Commit, txn, {}, {}, {}}); }
void LogManager::AppendAbort(std::uint64_t txn) { Append({LogType::Abort, txn, {}, {}, {}}); }
void LogManager::Flush() {
  std::ofstream out(file_, std::ios::binary | std::ios::app);
  if (!out) throw std::runtime_error("cannot open WAL for flush");
  out.flush();
  if (!out) throw std::runtime_error("WAL flush failed");
}
std::vector<LogRecord> LogManager::ReadAll() const { return records_; }

TransactionManager::TransactionManager(LogManager &log) : log_(log) {
  for (const auto &record : log_.ReadAll()) {
    next_ = std::max(next_, record.txn + 1);
    if (record.type == LogType::Begin) states_[record.txn] = TransactionState::Active;
    if (record.type == LogType::Commit) states_[record.txn] = TransactionState::Committed;
    if (record.type == LogType::Abort) states_[record.txn] = TransactionState::Aborted;
  }
}
std::uint64_t TransactionManager::BeginTransaction() {
  const auto txn = next_++;
  states_[txn] = TransactionState::Active;
  log_.AppendBegin(txn);
  return txn;
}
void TransactionManager::CommitTransaction(std::uint64_t txn) {
  if (State(txn) != TransactionState::Active) throw std::runtime_error("transaction is not active");
  log_.AppendCommit(txn); log_.Flush(); states_[txn] = TransactionState::Committed;
}
void TransactionManager::AbortTransaction(std::uint64_t txn) {
  if (State(txn) != TransactionState::Active) throw std::runtime_error("transaction is not active");
  log_.AppendAbort(txn); log_.Flush(); states_[txn] = TransactionState::Aborted;
}
void TransactionManager::LogInsert(std::uint64_t txn, RID rid, const Tuple &tuple) {
  if (State(txn) != TransactionState::Active) throw std::runtime_error("transaction is not active");
  log_.AppendInsert(txn, rid, tuple);
}
void TransactionManager::LogUpdate(std::uint64_t txn, RID rid, const Tuple &before,
                                   const Tuple &after) {
  if (State(txn) != TransactionState::Active) throw std::runtime_error("transaction is not active");
  log_.AppendUpdate(txn, rid, before, after);
}
TransactionState TransactionManager::State(std::uint64_t txn) const {
  auto it = states_.find(txn);
  return it == states_.end() ? TransactionState::Aborted : it->second;
}
void TransactionManager::Flush() { log_.Flush(); }

std::vector<LogRecord> RecoveryManager::Redo() const {
  std::map<std::uint64_t, bool> committed;
  for (const auto &record : log_.ReadAll()) {
    if (record.type == LogType::Begin) committed[record.txn] = false;
    if (record.type == LogType::Commit) committed[record.txn] = true;
  }
  std::vector<LogRecord> result;
  for (const auto &record : log_.ReadAll())
    if ((record.type == LogType::Insert || record.type == LogType::Update) &&
        committed[record.txn]) result.push_back(record);
  return result;
}
std::vector<std::uint64_t> RecoveryManager::Losers() const {
  std::map<std::uint64_t, bool> complete;
  for (const auto &record : log_.ReadAll()) {
    if (record.type == LogType::Begin) complete[record.txn] = false;
    if (record.type == LogType::Commit || record.type == LogType::Abort) complete[record.txn] = true;
  }
  std::vector<std::uint64_t> result;
  for (const auto &[txn, done] : complete) if (!done) result.push_back(txn);
  return result;
}
std::size_t RecoveryManager::Recover(TableHeap &heap) const {
  std::size_t applied = 0;
  for (const auto &record : Redo()) {
    try {
      if (record.type == LogType::Update) {
        auto tuple = DecodeTuple(record.after);
        if (heap.Get(record.rid).has_value()) {
          heap.Update(record.rid, tuple);
          ++applied;
        }
      } else if (record.type == LogType::Insert && !heap.Get(record.rid).has_value()) {
        heap.Insert(DecodeTuple(record.after));
        ++applied;
      }
    } catch (const std::exception &) {
      throw std::runtime_error("WAL recovery replay failed");
    }
  }
  return applied;
}

namespace {
std::string Upper(std::string value) {
  for (char &c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return value;
}
bool IsWordStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class SqlParser {
 public:
  explicit SqlParser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
  Statement Parse() {
    if (Peek().kind == TokenKind::End) Fail("empty SQL statement");
    Statement result{};
    if (IsKeyword("SELECT")) result = ParseSelect();
    else if (IsKeyword("INSERT")) result = ParseInsert();
    else if (IsKeyword("UPDATE")) result = ParseUpdate();
    else Fail("expected SELECT, INSERT, or UPDATE");
    if (Peek().kind == TokenKind::Semicolon) Consume();
    Expect(TokenKind::End, "unexpected trailing token");
    return result;
  }
 private:
  const Token &Peek() const { return tokens_[position_]; }
  Token Consume() { return tokens_[position_++]; }
  bool IsKeyword(std::string_view word) const {
    return Peek().kind == TokenKind::Keyword && Upper(Peek().text) == word;
  }
  void Expect(TokenKind kind, std::string_view message) {
    if (Peek().kind != kind) Fail(std::string(message) + " near '" + Peek().text + "'");
    Consume();
  }
  void Fail(const std::string &message) const { throw std::runtime_error("SQL parse error: " + message); }
  std::string Identifier(std::string_view context) {
    if (Peek().kind != TokenKind::Identifier) Fail(std::string("expected ") + std::string(context));
    return Consume().text;
  }
  std::string ValueText() {
    if (Peek().kind != TokenKind::Integer && Peek().kind != TokenKind::String &&
        !(Peek().kind == TokenKind::Identifier &&
          (Upper(Peek().text) == "TRUE" || Upper(Peek().text) == "FALSE")))
      Fail("expected integer or string literal");
    return Consume().text;
  }
  Statement ParseSelect() {
    Consume();
    Statement s{}; s.kind = Statement::Kind::Select;
    SelectStatement ast{};
    if (Peek().kind == TokenKind::Star) { Consume(); s.columns.push_back("*"); ast.columns = s.columns; }
    else {
      s.columns.push_back(Identifier("column name"));
      while (Peek().kind == TokenKind::Comma) { Consume(); s.columns.push_back(Identifier("column name after comma")); }
      ast.columns = s.columns;
    }
    if (!IsKeyword("FROM")) Fail("SELECT requires FROM");
    Consume(); s.table = Identifier("table name"); ast.table = s.table;
    if (IsKeyword("WHERE")) { Consume(); ast.where = ParseOr(s); }
    s.ast = std::move(ast);
    return s;
  }
  Statement ParseInsert() {
    Consume();
    Statement s{}; s.kind = Statement::Kind::Insert;
    InsertStatement ast{};
    if (!IsKeyword("INTO")) Fail("INSERT requires INTO");
    Consume(); s.table = Identifier("table name"); ast.table = s.table;
    if (!IsKeyword("VALUES")) Fail("INSERT requires VALUES");
    Consume(); Expect(TokenKind::LParen, "INSERT VALUES requires '('");
    s.values.push_back(ValueText());
    while (Peek().kind == TokenKind::Comma) { Consume(); s.values.push_back(ValueText()); }
    Expect(TokenKind::RParen, "INSERT VALUES requires ')'");
    ast.values = s.values; s.ast = std::move(ast); return s;
  }
  Statement ParseUpdate() {
    Consume();
    Statement s{}; s.kind = Statement::Kind::Update;
    UpdateStatement ast{};
    s.table = Identifier("table name"); ast.table = s.table;
    if (!IsKeyword("SET")) Fail("UPDATE requires SET");
    Consume(); s.update_column = Identifier("column name after SET");
    Expect(TokenKind::Equal, "UPDATE requires '='"); s.update_value = ValueText();
    ast.column = s.update_column; ast.value = s.update_value;
    if (IsKeyword("WHERE")) { Consume(); ast.where = ParseOr(s); }
    s.ast = std::move(ast); return s;
  }
  ExpressionPtr ParseOr(Statement &statement) {
    auto left = ParseAnd(statement);
    while (Peek().kind == TokenKind::Or) { Consume(); left = MakeLogical(ExpressionKind::Or, left, ParseAnd(statement)); if (!statement.predicate_or.empty()) statement.predicate_or.back() = true; }
    return left;
  }
  ExpressionPtr ParseAnd(Statement &statement) {
    auto left = ParsePrimary(statement);
    while (Peek().kind == TokenKind::And) { Consume(); left = MakeLogical(ExpressionKind::And, left, ParsePrimary(statement)); if (!statement.predicate_or.empty()) statement.predicate_or.back() = false; }
    return left;
  }
  ExpressionPtr ParsePrimary(Statement &statement) {
    if (Peek().kind == TokenKind::LParen) { Consume(); auto expression = ParseOr(statement); Expect(TokenKind::RParen, "missing ')'"); return expression; }
    const auto column = Identifier("column name in WHERE");
    const auto op = Consume();
    std::string op_text = op.text;
    if (op.kind != TokenKind::Equal && op.kind != TokenKind::NotEqual && op.kind != TokenKind::Less &&
        op.kind != TokenKind::Greater && op.kind != TokenKind::LessEqual && op.kind != TokenKind::GreaterEqual)
      Fail("expected comparison operator");
    const auto value = ValueText();
    statement.predicates.push_back({column, op_text, value});
    if (statement.predicates.size() > 1)
      statement.predicate_or.push_back(false);
    auto expression = std::make_shared<Expression>();
    expression->kind = ExpressionKind::Comparison; expression->text = op_text;
    expression->left = std::make_shared<Expression>(); expression->left->kind = ExpressionKind::Column; expression->left->text = column;
    expression->right = std::make_shared<Expression>();
    expression->right->kind = PeekValueIsInteger(value) ? ExpressionKind::IntegerLiteral : ExpressionKind::StringLiteral;
    expression->right->text = value;
    if (expression->right->kind == ExpressionKind::IntegerLiteral) expression->right->integer = std::stoi(value);
    return expression;
  }
  static bool PeekValueIsInteger(const std::string &value) {
    if (value.empty()) return false;
    std::size_t start = value[0] == '-' ? 1 : 0;
    return start < value.size() && std::all_of(value.begin() + static_cast<std::ptrdiff_t>(start), value.end(),
                                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
  }
  static ExpressionPtr MakeLogical(ExpressionKind kind, ExpressionPtr left, ExpressionPtr right) {
    auto result = std::make_shared<Expression>(); result->kind = kind; result->left = std::move(left); result->right = std::move(right); return result;
  }
  std::vector<Token> tokens_; std::size_t position_{0};
};
}

std::vector<Token> Lex(std::string_view sql) {
  std::vector<Token> tokens;
  for (std::size_t i = 0; i < sql.size();) {
    const char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (IsWordStart(c)) {
      const auto start = i++;
      while (i < sql.size() && IsWordChar(sql[i])) ++i;
      const std::string word(sql.substr(start, i - start));
      const auto upper = Upper(word);
      const auto kind = upper == "AND" ? TokenKind::And : upper == "OR" ? TokenKind::Or :
                        upper == "SELECT" || upper == "FROM" || upper == "WHERE" ||
                        upper == "INSERT" || upper == "INTO" || upper == "VALUES" ||
                        upper == "UPDATE" || upper == "SET" ? TokenKind::Keyword : TokenKind::Identifier;
      tokens.push_back({kind, word}); continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      const auto start = i++; while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
      tokens.push_back({TokenKind::Integer, std::string(sql.substr(start, i - start))}); continue;
    }
    if (c == '\'') {
      ++i; const auto start = i;
      while (i < sql.size() && sql[i] != '\'') { if (sql[i] == '\n' || sql[i] == '\r') throw std::runtime_error("SQL lex error: newline in string literal"); ++i; }
      if (i == sql.size()) throw std::runtime_error("SQL lex error: unterminated string literal");
      tokens.push_back({TokenKind::String, std::string(sql.substr(start, i - start))}); ++i; continue;
    }
    if (c == ',') tokens.push_back({TokenKind::Comma, ","});
    else if (c == '*') tokens.push_back({TokenKind::Star, "*"});
    else if (c == '(') tokens.push_back({TokenKind::LParen, "("});
    else if (c == ')') tokens.push_back({TokenKind::RParen, ")"});
    else if (c == ';') tokens.push_back({TokenKind::Semicolon, ";"});
    else if (c == '=') tokens.push_back({TokenKind::Equal, "="});
    else if (c == '!' && i + 1 < sql.size() && sql[i + 1] == '=') { tokens.push_back({TokenKind::NotEqual, "!="}); ++i; }
    else if (c == '<' && i + 1 < sql.size() && sql[i + 1] == '=') { tokens.push_back({TokenKind::LessEqual, "<="}); ++i; }
    else if (c == '>' && i + 1 < sql.size() && sql[i + 1] == '=') { tokens.push_back({TokenKind::GreaterEqual, ">="}); ++i; }
    else if (c == '<') tokens.push_back({TokenKind::Less, "<"});
    else if (c == '>') tokens.push_back({TokenKind::Greater, ">"});
    else throw std::runtime_error(std::string("SQL lex error: unexpected character '") + c + "'");
    ++i;
  }
  tokens.push_back({TokenKind::End, ""});
  return tokens;
}

Statement Parse(std::string_view sql) { return SqlParser(Lex(sql)).Parse(); }
namespace {
std::int32_t ParseInt(const std::string &text) {
  std::size_t used = 0;
  const auto value = std::stoll(text, &used);
  if (used != text.size() || value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::int32_t>::max())
    throw std::runtime_error("integer literal out of range");
  return static_cast<std::int32_t>(value);
}
}

Executor::Executor(TableHeap &h, const Schema &s, BPlusTree *i,
                   std::string table_name, TransactionManager *transactions)
    : heap_(h), schema_(s), index_(i), table_name_(std::move(table_name)),
      transactions_(transactions) {}

void Executor::CheckTable(std::string_view table) const {
  if (table != table_name_) throw std::runtime_error("unknown table: " + std::string(table));
}

Tuple Executor::MakeTuple(const std::vector<std::string> &values) const {
  if (values.size() != schema_.Columns().size())
    throw std::runtime_error("wrong number of INSERT values");
  std::vector<Value> result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    try {
      if (schema_.Columns()[i].type == Column::Type::Int32) result.emplace_back(ParseInt(values[i]));
      else if (schema_.Columns()[i].type == Column::Type::Boolean) {
        const auto value = Upper(values[i]);
        if (value != "TRUE" && value != "FALSE") throw std::runtime_error("expected TRUE or FALSE");
        result.emplace_back(value == "TRUE");
      } else {
        if (schema_.Columns()[i].not_null && values[i].empty())
          throw std::runtime_error("NULL value for NOT NULL column");
        result.emplace_back(values[i]);
      }
    } catch (const std::exception &) {
      throw std::runtime_error("incompatible value for column " + schema_.Columns()[i].name);
    }
  }
  return Tuple(std::move(result));
}

bool Executor::Matches(const Tuple &tuple, const ExpressionPtr &expression) const {
  if (!expression) return true;
  if (expression->kind == ExpressionKind::And)
    return Matches(tuple, expression->left) && Matches(tuple, expression->right);
  if (expression->kind == ExpressionKind::Or)
    return Matches(tuple, expression->left) || Matches(tuple, expression->right);
  if (expression->kind != ExpressionKind::Comparison || !expression->left || !expression->right)
    throw std::runtime_error("invalid predicate expression");
  const int column = schema_.IndexOf(expression->left->text);
  if (column < 0 || static_cast<std::size_t>(column) >= tuple.Values().size())
    throw std::runtime_error("unknown column: " + expression->left->text);
  const auto &actual = tuple.Get(static_cast<std::size_t>(column));
  bool result = false;
  if (schema_.Columns()[static_cast<std::size_t>(column)].type == Column::Type::Int32) {
    if (expression->right->kind != ExpressionKind::IntegerLiteral)
      throw std::runtime_error("integer column compared with string");
    const auto expected = expression->right->integer;
    const auto value = std::get<std::int32_t>(actual);
    result = expression->text == "=" ? value == expected : expression->text == "!=" ? value != expected :
             expression->text == "<" ? value < expected : expression->text == ">" ? value > expected :
             expression->text == "<=" ? value <= expected : value >= expected;
  } else if (schema_.Columns()[static_cast<std::size_t>(column)].type == Column::Type::Boolean) {
    if (expression->right->kind != ExpressionKind::StringLiteral)
      throw std::runtime_error("boolean column compared with non-boolean");
    const auto expected_text = Upper(expression->right->text);
    if (expected_text != "TRUE" && expected_text != "FALSE")
      throw std::runtime_error("boolean comparison requires TRUE or FALSE");
    const auto expected = expected_text == "TRUE";
    const auto value = std::get<bool>(actual);
    result = expression->text == "=" ? value == expected : expression->text == "!=" ? value != expected :
             expression->text == "<" ? value < expected : expression->text == ">" ? value > expected :
             expression->text == "<=" ? value <= expected : value >= expected;
  } else {
    if (expression->right->kind != ExpressionKind::StringLiteral)
      throw std::runtime_error("string column compared with integer");
    const auto &expected = expression->right->text;
    const auto &value = std::get<std::string>(actual);
    result = expression->text == "=" ? value == expected : expression->text == "!=" ? value != expected :
             expression->text == "<" ? value < expected : expression->text == ">" ? value > expected :
             expression->text == "<=" ? value <= expected : value >= expected;
  }
  return result;
}

QueryResult Executor::Execute(const Statement &stmt, const QueryPlan &plan) const {
  CheckTable(stmt.table);
  if (stmt.kind == Statement::Kind::Insert) {
    const auto rid = const_cast<Executor *>(this)->Insert(stmt);
    return {schema_.Columns().empty() ? std::vector<std::string>{} :
            std::vector<std::string>{"RID"}, {{static_cast<std::int32_t>(rid.page_id)}}};
  }
  if (stmt.kind == Statement::Kind::Update) {
    const auto count = const_cast<Executor *>(this)->Update(stmt);
    return {{"updated"}, {{static_cast<std::int32_t>(count)}}};
  }
  const auto &ast = std::get<SelectStatement>(stmt.ast);
  std::vector<std::pair<RID, Tuple>> rows;
  if (plan.kind == PlanKind::IndexScan && index_ && ast.where) {
    std::int32_t low = std::numeric_limits<std::int32_t>::min();
    std::int32_t high = std::numeric_limits<std::int32_t>::max();
    bool exact = false;
    std::function<void(const ExpressionPtr &)> bounds = [&](const ExpressionPtr &e) {
      if (!e) return;
      if (e->kind == ExpressionKind::And) { bounds(e->left); bounds(e->right); return; }
      if (e->kind == ExpressionKind::Or) {
        low = std::numeric_limits<std::int32_t>::min();
        high = std::numeric_limits<std::int32_t>::max();
        return;
      }
      if (e->kind != ExpressionKind::Comparison || e->left->text != schema_.Columns()[0].name) return;
      const auto value = e->right->integer;
      if (e->text == "=") { low = high = value; exact = true; }
      else if (e->text == ">") { if (value != std::numeric_limits<std::int32_t>::max()) low = std::max(low, value + 1); }
      else if (e->text == ">=") low = std::max(low, value);
      else if (e->text == "<") { if (value != std::numeric_limits<std::int32_t>::min()) high = std::min(high, value - 1); }
      else if (e->text == "<=") high = std::min(high, value);
    };
    bounds(ast.where);
    if (low <= high) for (const auto &[key, rid] : index_->Range(low, high))
      if (auto tuple = heap_.Get(rid)) rows.push_back({rid, *tuple});
    (void)exact;
  } else rows = heap_.Scan();
  QueryResult result;
  for (const auto &column : ast.columns) {
    if (column == "*") { for (const auto &c : schema_.Columns()) result.columns.push_back(c.name); break; }
    if (schema_.IndexOf(column) < 0) throw std::runtime_error("unknown column: " + column);
    result.columns.push_back(column);
  }
  for (const auto &[rid, tuple] : rows) {
    if (!Matches(tuple, ast.where)) continue;
    std::vector<Value> projected;
    for (const auto &column : ast.columns) {
      if (column == "*") { projected = tuple.Values(); break; }
      projected.push_back(tuple.Get(static_cast<std::size_t>(schema_.IndexOf(column))));
    }
    result.rows.push_back(std::move(projected));
  }
  return result;
}

QueryResult Executor::ExecuteSql(std::string_view sql, const QueryPlan &plan) const {
  const auto statement = Parse(sql);
  return Execute(statement, plan.kind == PlanKind::SequentialScan && plan.use_filter == false &&
                         plan.use_projection == false ? Planner(schema_, index_, table_name_).Plan(statement) : plan);
}

std::vector<Tuple> Executor::Select(const Statement &stmt) const {
  const auto result = Execute(stmt, Planner(schema_, index_, table_name_).Plan(stmt));
  std::vector<Tuple> rows;
  for (const auto &values : result.rows) rows.emplace_back(values);
  return rows;
}

RID Executor::Insert(const Statement &stmt) {
  const auto tuple = MakeTuple(stmt.values);
  if (index_) {
    const auto key = std::get<std::int32_t>(tuple.Get(0));
    if (index_->Search(key).has_value())
      throw std::runtime_error("duplicate primary key");
  }
  std::uint64_t txn = 0;
  if (transactions_) txn = transactions_->BeginTransaction();
  const RID rid = heap_.Insert(tuple);
  if (index_ && !index_->Insert(std::get<std::int32_t>(tuple.Get(0)), rid)) {
    heap_.Delete(rid);
    throw std::runtime_error("duplicate primary key");
  }
  if (transactions_) { transactions_->LogInsert(txn, rid, tuple); transactions_->CommitTransaction(txn); }
  return rid;
}

std::size_t Executor::Update(const Statement &stmt) {
  const int column = schema_.IndexOf(stmt.update_column);
  if (column < 0) throw std::runtime_error("unknown column: " + stmt.update_column);
  if (index_ && column == 0) throw std::runtime_error("updating indexed key is unsupported");
  Value replacement;
  if (schema_.Columns()[static_cast<std::size_t>(column)].type == Column::Type::Int32)
    replacement = ParseInt(stmt.update_value);
  else if (schema_.Columns()[static_cast<std::size_t>(column)].type == Column::Type::Boolean) {
    const auto value = Upper(stmt.update_value);
    if (value != "TRUE" && value != "FALSE") throw std::runtime_error("expected TRUE or FALSE");
    replacement = value == "TRUE";
  } else replacement = stmt.update_value;
  std::uint64_t txn = 0;
  if (transactions_) txn = transactions_->BeginTransaction();
  std::size_t count = 0;
  std::vector<std::pair<RID, Tuple>> candidates;
  const auto plan = Planner(schema_, index_, table_name_).Plan(stmt);
  if (plan.kind == PlanKind::IndexScan && index_) {
    std::int32_t low = std::numeric_limits<std::int32_t>::min();
    std::int32_t high = std::numeric_limits<std::int32_t>::max();
    const auto &where = std::get<UpdateStatement>(stmt.ast).where;
    std::function<void(const ExpressionPtr &)> bounds = [&](const ExpressionPtr &e) {
      if (!e) return;
      if (e->kind == ExpressionKind::And) { bounds(e->left); bounds(e->right); return; }
      if (e->kind == ExpressionKind::Or) { low = std::numeric_limits<std::int32_t>::min(); high = std::numeric_limits<std::int32_t>::max(); return; }
      if (e->kind != ExpressionKind::Comparison || e->left->text != schema_.Columns()[0].name) return;
      const auto value = e->right->integer;
      if (e->text == "=") low = high = value;
      else if (e->text == ">=") low = std::max(low, value);
      else if (e->text == ">" && value != std::numeric_limits<std::int32_t>::max()) low = std::max(low, value + 1);
      else if (e->text == "<=") high = std::min(high, value);
      else if (e->text == "<" && value != std::numeric_limits<std::int32_t>::min()) high = std::min(high, value - 1);
    };
    bounds(where);
    if (low <= high) for (const auto &[key, rid] : index_->Range(low, high))
      if (auto tuple = heap_.Get(rid)) candidates.push_back({rid, *tuple});
  } else {
    candidates = heap_.Scan();
  }
  for (const auto &[rid, tuple] : candidates) {
    const auto &where = std::get<UpdateStatement>(stmt.ast).where;
    if (!Matches(tuple, where)) continue;
    auto values = tuple.Values(); values[static_cast<std::size_t>(column)] = replacement;
    Tuple updated(std::move(values));
    if (transactions_) transactions_->LogUpdate(txn, rid, tuple, updated);
    if (heap_.Update(rid, updated)) ++count;
  }
  if (transactions_) transactions_->CommitTransaction(txn);
  return count;
}

QueryPlan Planner::Plan(const Statement &stmt) const {
  QueryPlan result{};
  if (!index_ || (stmt.kind != Statement::Kind::Select && stmt.kind != Statement::Kind::Update))
    return result;
  const auto &where = stmt.kind == Statement::Kind::Select
      ? std::get<SelectStatement>(stmt.ast).where : std::get<UpdateStatement>(stmt.ast).where;
  std::function<bool(const ExpressionPtr &)> usable = [&](const ExpressionPtr &e) {
    if (!e) return false;
    if (e->kind == ExpressionKind::And) return usable(e->left) || usable(e->right);
    if (e->kind == ExpressionKind::Or) return usable(e->left) && usable(e->right);
    return e->kind == ExpressionKind::Comparison && e->left &&
           e->left->text == schema_.Columns()[0].name &&
           e->right && e->right->kind == ExpressionKind::IntegerLiteral;
  };
  if (usable(where)) result.kind = PlanKind::IndexScan;
  return result;
}

std::size_t Executor::Delete(const Statement &stmt) {
  if (stmt.kind != Statement::Kind::Select)
    throw std::runtime_error("DELETE requires a parsed predicate statement");
  const auto rows = heap_.Scan();
  std::size_t count = 0;
  for (const auto &[rid, tuple] : rows) {
    if (Matches(tuple, std::get<SelectStatement>(stmt.ast).where) &&
        heap_.Delete(rid)) {
      ++count;
    }
  }
  return count;
}

namespace {
bool ValidName(std::string_view name) {
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) ||
                        name.front() == '_')) return false;
  return std::all_of(name.begin() + 1, name.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  });
}

std::vector<std::string> Words(std::string_view sql) {
  std::vector<std::string> result;
  std::string current;
  for (char c : sql) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') current += c;
    else if (!current.empty()) { result.push_back(Upper(current)); current.clear(); }
  }
  if (!current.empty()) result.push_back(Upper(current));
  return result;
}

std::string CatalogLine(const TableDefinition &table, page_id_t first, page_id_t root) {
  std::string line = table.name + "|" + std::to_string(first) + "|" + std::to_string(root) + "|";
  for (std::size_t i = 0; i < table.schema.Columns().size(); ++i) {
    if (i) line += ",";
    const auto &column = table.schema.Columns()[i];
    const auto type = column.type == Column::Type::Int32 ? "INT" :
                      column.type == Column::Type::Varchar ? "VARCHAR" : "BOOLEAN";
    line += column.name + ":" + type +
            ":" + (column.not_null ? "N" : "-") + (column.primary_key ? "P" : "-");
  }
  return line;
}

TableDefinition ParseCreateTable(const std::vector<Token> &tokens) {
  std::size_t p = 0;
  auto upper = [&](const std::string &value) { return Upper(value); };
  auto expect_word = [&](const char *word) {
    if (p >= tokens.size() || upper(tokens[p++].text) != word)
      throw std::runtime_error(std::string("expected ") + word);
  };
  auto expect = [&](TokenKind kind, const char *message) {
    if (p >= tokens.size() || tokens[p].kind != kind) throw std::runtime_error(message);
    ++p;
  };
  expect_word("CREATE");
  expect_word("TABLE");
  if (p >= tokens.size() || tokens[p].kind != TokenKind::Identifier)
    throw std::runtime_error("CREATE TABLE requires a table name");
  TableDefinition result;
  result.name = tokens[p++].text;
  expect(TokenKind::LParen, "CREATE TABLE requires '('");
  std::vector<Column> columns;
  while (p < tokens.size() && tokens[p].kind != TokenKind::RParen) {
    if (tokens[p].kind != TokenKind::Identifier)
      throw std::runtime_error("expected column name");
    Column column;
    column.name = tokens[p++].text;
    if (p >= tokens.size()) throw std::runtime_error("missing column type");
    const auto type = upper(tokens[p++].text);
    if (type == "INT") column.type = Column::Type::Int32;
    else if (type == "VARCHAR") column.type = Column::Type::Varchar;
    else if (type == "BOOLEAN") column.type = Column::Type::Boolean;
    else throw std::runtime_error("supported types are INT, VARCHAR, and BOOLEAN");
    if (p < tokens.size() && tokens[p].kind == TokenKind::LParen) {
      ++p;
      if (p >= tokens.size() || tokens[p].kind != TokenKind::Integer)
        throw std::runtime_error("VARCHAR length is invalid");
      ++p;
      expect(TokenKind::RParen, "VARCHAR length requires ')'");
    }
    while (p < tokens.size() && tokens[p].kind != TokenKind::Comma &&
           tokens[p].kind != TokenKind::RParen) {
      const auto constraint = upper(tokens[p++].text);
      if (constraint == "PRIMARY") {
        expect_word("KEY");
        column.primary_key = true;
      } else if (constraint == "NOT") {
        expect_word("NULL");
        column.not_null = true;
      } else throw std::runtime_error("unsupported column constraint");
    }
    columns.push_back(column);
    if (p < tokens.size() && tokens[p].kind == TokenKind::Comma) ++p;
  }
  expect(TokenKind::RParen, "CREATE TABLE requires ')'");
  result.schema = Schema(std::move(columns));
  return result;
}

std::string RebuildInsertSql(const std::string &table, const std::vector<Token> &values) {
  std::string sql = "INSERT INTO " + table + " VALUES (";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) sql += ", ";
    if (values[i].kind == TokenKind::String) sql += "'" + values[i].text + "'";
    else sql += values[i].text;
  }
  return sql + ");";
}

struct SelectModifiers {
  std::string base_sql;
  std::string order_column;
  bool descending{false};
  std::optional<std::size_t> limit;
  bool count_rows{false};
};

SelectModifiers ParseSelectModifiers(const std::string &sql) {
  SelectModifiers result{sql, {}, false, std::nullopt, false};
  const auto upper = Upper(sql);
  const auto order = upper.find("ORDER BY");
  const auto limit = upper.find("LIMIT");
  std::size_t clause = std::string::npos;
  if (order != std::string::npos) clause = order;
  if (limit != std::string::npos && (clause == std::string::npos || limit < clause))
    clause = limit;
  if (clause != std::string::npos) {
    result.base_sql = sql.substr(0, clause) + ";";
    const auto order_end = limit != std::string::npos && limit > clause ? limit : sql.size();
    if (clause == order) {
      const auto order_tokens = Lex(sql.substr(order + 8, order_end - order - 8));
      if (order_tokens.empty() || order_tokens[0].kind != TokenKind::Identifier)
        throw std::runtime_error("ORDER BY requires a column name");
      result.order_column = order_tokens[0].text;
      if (order_tokens.size() > 1 && order_tokens[1].kind != TokenKind::End &&
          order_tokens[1].kind != TokenKind::Semicolon) {
        const auto direction = Upper(order_tokens[1].text);
        if (direction == "DESC") result.descending = true;
        else if (direction != "ASC")
          throw std::runtime_error("ORDER BY direction must be ASC or DESC");
      }
    }
    if (limit != std::string::npos) {
      const auto limit_tokens = Lex(sql.substr(limit + 5));
      if (limit_tokens.empty() || limit_tokens[0].kind != TokenKind::Integer)
        throw std::runtime_error("LIMIT requires a non-negative integer");
      const auto value = std::stoll(limit_tokens[0].text);
      if (value < 0) throw std::runtime_error("LIMIT requires a non-negative integer");
      result.limit = static_cast<std::size_t>(value);
    }
  }
  const auto count = upper.find("COUNT(*)");
  const auto from = upper.find("FROM");
  if (count != std::string::npos && from != std::string::npos && count < from) {
    const auto count_in_base = Upper(result.base_sql).find("COUNT(*)");
    if (count_in_base != std::string::npos)
      result.base_sql = result.base_sql.substr(0, count_in_base) + "*" +
                        result.base_sql.substr(count_in_base + 8);
    result.count_rows = true;
  }
  return result;
}
}

struct Database::TableContext {
  std::unique_ptr<LogManager> log;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<BufferPoolManager> buffer;
  std::unique_ptr<TableHeap> heap;
  std::unique_ptr<BPlusTree> index;
  std::unique_ptr<TransactionManager> transactions;
  std::unique_ptr<Executor> executor;
  TableContext(const std::filesystem::path &directory, const TableDefinition &definition,
               page_id_t first, page_id_t root)
      : log(std::make_unique<LogManager>((directory / (definition.name + ".wal")).string())),
        disk(std::make_unique<DiskManager>((directory / (definition.name + ".db")).string())),
        buffer(std::make_unique<BufferPoolManager>(*disk, log.get())),
        heap(std::make_unique<TableHeap>(*buffer, first)),
        index(std::make_unique<BPlusTree>(*buffer, root)),
        transactions(std::make_unique<TransactionManager>(*log)),
        executor(std::make_unique<Executor>(*heap, definition.schema, index.get(),
                                             definition.name, transactions.get())) {}
};

Database::Database(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)) {
  std::filesystem::create_directories(data_directory_);
}

Database::~Database() { try { Flush(); } catch (...) {} }

void Database::RequireDatabase() const {
  if (current_database_.empty()) throw std::runtime_error("no database selected; use USE database");
}

std::vector<std::string> Database::ShowDatabases() const {
  std::vector<std::string> result;
  for (const auto &entry : std::filesystem::directory_iterator(data_directory_))
    if (entry.is_directory()) result.push_back(entry.path().filename().string());
  std::sort(result.begin(), result.end());
  return result;
}

void Database::CreateDatabase(const std::string &name) {
  if (!ValidName(name)) throw std::runtime_error("invalid database name");
  const auto path = data_directory_ / name;
  if (std::filesystem::exists(path)) throw std::runtime_error("database already exists: " + name);
  std::filesystem::create_directories(path);
  std::ofstream(path / "catalog.meta").close();
}

void Database::DropDatabase(const std::string &name) {
  if (!ValidName(name)) throw std::runtime_error("invalid database name");
  if (name == current_database_) { tables_.clear(); catalog_.clear(); current_database_.clear(); }
  const auto path = data_directory_ / name;
  if (!std::filesystem::exists(path)) throw std::runtime_error("unknown database: " + name);
  std::filesystem::remove_all(path);
}

void Database::LoadCatalog() {
  catalog_.clear(); tables_.clear();
  std::ifstream input(data_directory_ / current_database_ / "catalog.meta");
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields; std::size_t start = 0;
    for (;;) {
      const auto end = line.find('|', start);
      fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
      if (end == std::string::npos) break;
      start = end + 1;
    }
    if (fields.size() != 4) throw std::runtime_error("corrupt catalog metadata");
    TableDefinition definition; definition.name = fields[0];
    std::size_t pos = 0;
    while (pos < fields[3].size()) {
      const auto comma = fields[3].find(',', pos);
      const auto item = fields[3].substr(pos, comma == std::string::npos ? comma : comma - pos);
      const auto colon = item.find(':');
      if (colon == std::string::npos) throw std::runtime_error("corrupt catalog schema");
      const auto second_colon = item.find(':', colon + 1);
      const auto type_name = item.substr(colon + 1, second_colon == std::string::npos
                                                        ? second_colon : second_colon - colon - 1);
      const auto flags = second_colon == std::string::npos ? "" : item.substr(second_colon + 1);
      definition.schema = Schema([&] {
        auto columns = definition.schema.Columns();
        columns.push_back({item.substr(0, colon),
          Upper(type_name) == "INT" ? Column::Type::Int32 :
          Upper(type_name) == "BOOLEAN" ? Column::Type::Boolean : Column::Type::Varchar,
          flags.find('N') != std::string::npos,
          flags.find('P') != std::string::npos});
        return columns;
      }());
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    catalog_[definition.name] = std::move(definition);
  }
}

void Database::SaveCatalog() const {
  std::ofstream output(data_directory_ / current_database_ / "catalog.meta",
                       std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write catalog");
  for (const auto &[name, definition] : catalog_) {
    page_id_t first = INVALID_PAGE_ID, root = INVALID_PAGE_ID;
    if (const auto it = tables_.find(name); it != tables_.end()) {
      first = it->second->heap->FirstPageId();
      root = it->second->index->RootPageId();
    } else {
      std::ifstream metadata(data_directory_ / current_database_ / (name + ".meta"));
      metadata >> first >> root;
    }
    output << CatalogLine(definition, first, root) << '\n';
  }
}

void Database::UseDatabase(const std::string &name) {
  if (!ValidName(name) || !std::filesystem::is_directory(data_directory_ / name))
    throw std::runtime_error("unknown database: " + name);
  Flush();
  current_database_ = name;
  LoadCatalog();
}

Database::TableContext &Database::OpenTable(const std::string &name) {
  RequireDatabase();
  const auto it = catalog_.find(name);
  if (it == catalog_.end()) throw std::runtime_error("unknown table: " + name);
  if (const auto found = tables_.find(name); found != tables_.end()) return *found->second;
  page_id_t first = INVALID_PAGE_ID, root = INVALID_PAGE_ID;
  std::ifstream metadata(data_directory_ / current_database_ / (name + ".meta"));
  if (metadata) metadata >> first >> root;
  auto context = std::make_unique<TableContext>(
      data_directory_ / current_database_, it->second, first, root);
  auto &result = *context;
  tables_[name] = std::move(context);
  return result;
}

void Database::CreateTable(const TableDefinition &definition) {
  RequireDatabase();
  if (!ValidName(definition.name) || definition.schema.Columns().empty())
    throw std::runtime_error("invalid table definition");
  if (catalog_.count(definition.name)) throw std::runtime_error("table already exists");
  std::set<std::string> names;
  bool primary_key_seen = false;
  for (std::size_t i = 0; i < definition.schema.Columns().size(); ++i) {
    const auto &column = definition.schema.Columns()[i];
    if (!ValidName(column.name) || !names.insert(column.name).second)
      throw std::runtime_error("invalid or duplicate column name");
    if (column.primary_key) {
      if (primary_key_seen || i != 0 || column.type != Column::Type::Int32)
        throw std::runtime_error("the first INT column must be the only primary key");
      primary_key_seen = true;
    }

  }
  catalog_[definition.name] = definition;
  auto &table = OpenTable(definition.name);
  table.buffer->FlushAllPages();
  std::ofstream metadata(data_directory_ / current_database_ / (definition.name + ".meta"),
                         std::ios::trunc);
  metadata << table.heap->FirstPageId() << ' ' << table.index->RootPageId() << '\n';
  SaveCatalog();
}

void Database::AddColumn(const std::string &name, Column column) {
  RequireDatabase();
  if (!ValidName(column.name) || catalog_.count(name) == 0)
    throw std::runtime_error("unknown table or invalid column");
  auto &definition = catalog_.at(name);
  if (definition.schema.IndexOf(column.name) >= 0)
    throw std::runtime_error("column already exists: " + column.name);
  auto &context = OpenTable(name);
  const Value default_value = column.type == Column::Type::Int32
      ? Value(std::int32_t{0}) : column.type == Column::Type::Boolean
      ? Value(false) : Value(std::string{});
  for (const auto &[rid, tuple] : context.heap->Scan()) {
    auto values = tuple.Values();
    values.push_back(default_value);
    if (!context.heap->Update(rid, Tuple(std::move(values))))
      throw std::runtime_error("failed to rewrite table for ALTER TABLE");
  }
  auto columns = definition.schema.Columns();
  columns.push_back(column);
  definition.schema = Schema(std::move(columns));
  context.executor = std::make_unique<Executor>(*context.heap, definition.schema,
                                                  context.index.get(), name,
                                                  context.transactions.get());
  SaveCatalog();
}

void Database::DropTable(const std::string &name) {
  RequireDatabase();
  if (!catalog_.count(name)) throw std::runtime_error("unknown table: " + name);
  tables_.erase(name); catalog_.erase(name);
  const auto directory = data_directory_ / current_database_;
  std::filesystem::remove(directory / (name + ".db"));
  std::filesystem::remove(directory / (name + ".wal"));
  std::filesystem::remove(directory / (name + ".meta"));
  SaveCatalog();
}

std::vector<std::string> Database::ShowTables() const {
  RequireDatabase();
  std::vector<std::string> result;
  for (const auto &[name, definition] : catalog_) result.push_back(name);
  std::sort(result.begin(), result.end());
  return result;
}

TableDefinition Database::Describe(const std::string &name) const {
  RequireDatabase();
  const auto it = catalog_.find(name);
  if (it == catalog_.end()) throw std::runtime_error("unknown table: " + name);
  return it->second;
}

QueryResult Database::ExecuteSql(std::string_view sql) {
  const auto text = std::string(sql);
  const auto tokens = Lex(text);
  const auto words = Words(text);
  auto message = [](const std::string &column, const std::string &value) {
    return QueryResult{{column}, {{value}}};
  };
  if (words.empty()) throw std::runtime_error("empty SQL statement");
  if (words[0] == "CREATE" && words.size() >= 2 && words[1] == "DATABASE") {
    if (tokens.size() < 3) throw std::runtime_error("CREATE DATABASE requires a name");
    CreateDatabase(tokens[2].text);
    return message("status", "database created");
  }
  if (words[0] == "SHOW" && words.size() >= 2 && words[1] == "DATABASES") {
    QueryResult result{{"database"}, {}};
    for (const auto &name : ShowDatabases()) result.rows.push_back({name});
    return result;
  }
  if (words[0] == "USE") {
    if (tokens.size() < 2) throw std::runtime_error("USE requires a database name");
    UseDatabase(tokens[1].text);
    return message("status", "using " + current_database_);
  }
  if (words[0] == "DROP" && words.size() >= 2 && words[1] == "DATABASE") {
    if (tokens.size() < 3) throw std::runtime_error("DROP DATABASE requires a name");
    DropDatabase(tokens[2].text);
    return message("status", "database dropped");
  }
  RequireDatabase();
  if (words[0] == "SHOW" && words.size() >= 2 && words[1] == "TABLES") {
    QueryResult result{{"table"}, {}};
    for (const auto &name : ShowTables()) result.rows.push_back({name});
    return result;
  }
  if ((words[0] == "DESCRIBE" || words[0] == "DESC")) {
    if (tokens.size() < 2) throw std::runtime_error("DESCRIBE requires a table name");
    const auto definition = Describe(tokens[1].text);
    QueryResult result{{"column", "type", "constraints"}, {}};
    for (const auto &column : definition.schema.Columns()) {
      std::string constraints;
      if (column.primary_key) constraints += "PRIMARY KEY ";
      if (column.not_null) constraints += "NOT NULL";
      result.rows.push_back({column.name,
        column.type == Column::Type::Int32 ? "INT" :
        column.type == Column::Type::Boolean ? "BOOLEAN" : "VARCHAR", constraints});
    }
    return result;
  }
  if (words[0] == "CREATE" && words.size() >= 2 && words[1] == "TABLE")
    { CreateTable(ParseCreateTable(tokens)); return message("status", "table created"); }
  if (words[0] == "DROP" && words.size() >= 2 && words[1] == "TABLE") {
    if (tokens.size() < 3) throw std::runtime_error("DROP TABLE requires a name");
    DropTable(tokens[2].text);
    return message("status", "table dropped");
  }
  if (words[0] == "ALTER" && words.size() >= 5 && words[1] == "TABLE" &&
      words[3] == "ADD" && words[4] == "COLUMN") {
    if (tokens.size() < 7) throw std::runtime_error("ALTER TABLE ADD COLUMN is incomplete");
    const auto type = Upper(tokens[6].text);
    Column column{tokens[5].text, type == "INT" ? Column::Type::Int32 :
        type == "BOOLEAN" ? Column::Type::Boolean : Column::Type::Varchar};
    if (type != "INT" && type != "VARCHAR" && type != "BOOLEAN")
      throw std::runtime_error("supported types are INT, VARCHAR, and BOOLEAN");
    AddColumn(tokens[2].text, column);
    return message("status", "column added");
  }
  if (words[0] == "INSERT") {
    if (tokens.size() < 6 || words.size() < 4 || words[1] != "INTO" ||
        words[3] != "VALUES")
      throw std::runtime_error("invalid INSERT statement");
    const auto table = tokens[2].text;
    auto &context = OpenTable(table);
    std::size_t p = 4, inserted = 0;
    while (p < tokens.size() && tokens[p].kind != TokenKind::End) {
      if (tokens[p].kind == TokenKind::Semicolon) { ++p; continue; }
      if (tokens[p].kind == TokenKind::Comma) { ++p; continue; }
      if (tokens[p].kind != TokenKind::LParen)
        throw std::runtime_error("INSERT VALUES requires '('");
      ++p;
      std::vector<Token> values;
      while (p < tokens.size() && tokens[p].kind != TokenKind::RParen) {
        if (tokens[p].kind != TokenKind::Integer && tokens[p].kind != TokenKind::String &&
            !(tokens[p].kind == TokenKind::Identifier &&
              (Upper(tokens[p].text) == "TRUE" || Upper(tokens[p].text) == "FALSE")))
          throw std::runtime_error("INSERT accepts integer and string literals");
        values.push_back(tokens[p++]);
        if (p < tokens.size() && tokens[p].kind == TokenKind::Comma) ++p;
      }
      if (p >= tokens.size()) throw std::runtime_error("INSERT VALUES requires ')'");
      ++p;
      context.executor->ExecuteSql(RebuildInsertSql(table, values));
      ++inserted;
    }
    context.buffer->FlushAllPages(); SaveCatalog();
    return {{"inserted"}, {{static_cast<std::int32_t>(inserted)}}};
  }
  if (words[0] == "DELETE" && words.size() >= 2 && words[1] == "FROM") {
    const auto table = tokens.size() > 2 ? tokens[2].text : "";
    auto &context = OpenTable(table);
    std::string translated = "SELECT * FROM " + table;
    auto upper = Upper(text);
    const auto where = upper.find("WHERE");
    if (where != std::string::npos) translated += " " + text.substr(where);
    auto statement = Parse(translated);
    const auto count = context.executor->Delete(statement);
    context.buffer->FlushAllPages(); SaveCatalog();
    return {{"deleted"}, {{static_cast<std::int32_t>(count)}}};
  }
  auto modifiers = ParseSelectModifiers(text);
  auto statement = Parse(modifiers.base_sql);
  auto &context = OpenTable(statement.table);
  auto result = context.executor->Execute(statement);
  if (modifiers.count_rows) {
    return {{"count"}, {{static_cast<std::int32_t>(result.rows.size())}}};
  }
  if (!modifiers.order_column.empty()) {
    const auto column = std::find(result.columns.begin(), result.columns.end(),
                                  modifiers.order_column);
    if (column == result.columns.end())
      throw std::runtime_error("ORDER BY column must be selected");
    const auto index = static_cast<std::size_t>(column - result.columns.begin());
    std::stable_sort(result.rows.begin(), result.rows.end(),
      [index, descending = modifiers.descending](const auto &left, const auto &right) {
        const auto &a = left.at(index);
        const auto &b = right.at(index);
        const bool less = std::visit([](const auto &x, const auto &y) {
          using X = std::decay_t<decltype(x)>;
          using Y = std::decay_t<decltype(y)>;
          if constexpr (std::is_same_v<X, Y>) return x < y;
          else return std::is_same_v<X, std::int32_t> ||
                      (std::is_same_v<X, bool> && std::is_same_v<Y, std::string>);
        }, a, b);
        const bool equal = std::visit([](const auto &x, const auto &y) {
          using X = std::decay_t<decltype(x)>;
          using Y = std::decay_t<decltype(y)>;
          if constexpr (std::is_same_v<X, Y>) return x == y;
          else return false;
        }, a, b);
        return descending ? !less && !equal : less;
      });
  }
  if (modifiers.limit && result.rows.size() > *modifiers.limit)
    result.rows.resize(*modifiers.limit);
  context.buffer->FlushAllPages(); SaveCatalog();
  return result;
}

void Database::Flush() {
  for (auto &[name, context] : tables_) {
    context->buffer->FlushAllPages();
    context->transactions->Flush();
    std::ofstream metadata(data_directory_ / current_database_ / (name + ".meta"),
                           std::ios::trunc);
    metadata << context->heap->FirstPageId() << ' ' << context->index->RootPageId() << '\n';
  }
  if (!current_database_.empty()) SaveCatalog();
}
} 
