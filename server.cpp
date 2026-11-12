#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <chrono>
#include <cstdlib> // getenv: optional cloud host and persistent data directory
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

namespace asio=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
namespace json=boost::json;
using tcp=asio::ip::tcp;
using Object=json::object;
using Array=json::array;

static std::filesystem::path base;
// Keep website assets in base; database and uploads may live on a mounted cloud disk.
static std::filesystem::path dataDir;
static json::value state;
static sqlite3* db=nullptr; // Open SQLite database connection for this process.
static std::unordered_map<std::string,std::string> sessions;
static std::mutex guard;
static uint64_t nextId=1000;

// Read a string field from a JSON object; use a fallback if missing or wrong type.
std::string str(const Object& o, std::string_view key, std::string def="") {
    auto it=o.find(key);return it!=o.end()&&it->value().is_string()?std::string(it->value().as_string()):def;
}
// Read an integer JSON field safely; IDs and numeric request parameters use this helper.
int64_t num(const Object& o,std::string_view key,int64_t def=-1){
    auto it=o.find(key);return it!=o.end()?(it->value().is_int64()?it->value().as_int64():(it->value().is_uint64()?static_cast<int64_t>(it->value().as_uint64()):def)):def;
}
// For this demo the teacher can modify courses 1–3 only.
bool validCourse(int64_t n){return n>=1&&n<=3;}
// Format the local calendar date for new student submissions.
std::string nowDate(){
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char buf[11];std::strftime(buf,sizeof(buf),"%Y-%m-%d",&tm);return buf;
}
// Create a random-looking identifier for a demo session or uploaded file.
std::string randomToken(){
    std::random_device rd;std::ostringstream ss;ss<<std::hex;
    for(int i=0;i<8;++i)ss<<static_cast<uint32_t>(rd());
    return ss.str();
}
// Demo attachments: max 4 MiB; binary stored on disk, metadata in SQLite.
// Restrict extensions and never use user-controlled paths on disk.
static constexpr size_t maxFileBytes=4*1024*1024;
// Decode the base64 content uploaded by the browser, enforcing the 4 MiB limit.
std::string decodeBase64(const std::string& encoded){
    if(encoded.empty()||encoded.size()%4!=0||encoded.size()>((maxFileBytes+2)/3)*4+4)
        throw std::runtime_error("Invalid file size");
    std::array<int,256> map;map.fill(-1);
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int i=0;i<64;i++)map[static_cast<unsigned char>(alphabet[i])]=i;
    std::string output;output.reserve(encoded.size()*3/4);
    for(size_t i=0;i<encoded.size();i+=4){
        int n=0,pad=0;
        for(int j=0;j<4;j++){
            char c=encoded[i+j];
            if(c=='='){
                if(i+4!=encoded.size()||j<2)throw std::runtime_error("Invalid base64 padding");
                pad++;n<<=6;
            }else{
                if(pad||map[static_cast<unsigned char>(c)]<0)throw std::runtime_error("Invalid base64 data");
                n=(n<<6)|map[static_cast<unsigned char>(c)];
            }
        }
        output.push_back(static_cast<char>((n>>16)&255));
        if(pad<2)output.push_back(static_cast<char>((n>>8)&255));
        if(pad<1)output.push_back(static_cast<char>(n&255));
        if(output.size()>maxFileBytes)throw std::runtime_error("File exceeds 4 MB limit");
    }
    return output;
}
// Validate a filename and extension, save the file to uploads/, and return its JSON metadata.
Object saveAttachment(const json::value& file){
    if(!file.is_object())throw std::runtime_error("Invalid attachment");
    auto& o=file.as_object();std::string name=str(o,"name"),encoded=str(o,"data");
    if(name.empty()||name.size()>180||name.find('/')!=std::string::npos||name.find('\\')!=std::string::npos)
        throw std::runtime_error("Invalid filename");
    auto dot=name.rfind('.');if(dot==std::string::npos)throw std::runtime_error("File extension required");
    std::string ext=name.substr(dot+1);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    const std::string allowed=" pdf doc docx ppt pptx xls xlsx txt md csv zip png jpg jpeg gif webp cpp h hpp c py java json sql ";
    if(allowed.find(" "+ext+" ")==std::string::npos)throw std::runtime_error("Unsupported file type");
    auto content=decodeBase64(encoded);
    std::string id=randomToken()+"."+ext;
    std::filesystem::create_directories(dataDir/"uploads");
    std::ofstream out(dataDir/"uploads"/id,std::ios::binary|std::ios::trunc);
    out.write(content.data(),static_cast<std::streamsize>(content.size()));out.close();
    if(!out)throw std::runtime_error("Cannot save attachment");
    return Object{{"id",id},{"name",name},{"size",static_cast<int64_t>(content.size())}};
}
// Check whether the current role is permitted to download a referenced attachment.
bool canDownload(const std::string& id,const std::string& role){
    for(const auto& task:state.as_object().at("tasks").as_array()){
        const auto& t=task.as_object();auto it=t.find("file");
        if(it!=t.end()&&it->value().is_object()&&str(it->value().as_object(),"id")==id)
            return role=="student"||validCourse(num(t,"course"));
    }
    for(const auto& sub:state.as_object().at("submissions").as_array()){
        const auto& s=sub.as_object();auto it=s.find("file");
        if(it==s.end()||!it->value().is_object()||str(it->value().as_object(),"id")!=id)continue;
        if(role=="student")return str(s,"student")=="Laurynas K.";
        for(const auto& t:state.as_object().at("tasks").as_array())
            if(num(t.as_object(),"id")==num(s,"task"))return validCourse(num(t.as_object(),"course"));
    }
    return false;
}
// Create initial demonstration records for a brand-new SQLite database.
json::value seed(){
    return json::parse(R"JSON({
      "schedule":[{"id":401,"day":0,"course":1,"start":"08:30","end":"10:00","room":"A-214"},{"id":402,"day":0,"course":2,"start":"10:15","end":"11:45","room":"B-103"},{"id":403,"day":1,"course":5,"start":"10:15","end":"11:45","room":"A-101"},{"id":404,"day":1,"course":3,"start":"12:30","end":"14:00","room":"C-305"},{"id":405,"day":2,"course":6,"start":"08:30","end":"10:00","room":"D-12"},{"id":406,"day":2,"course":1,"start":"12:30","end":"14:00","room":"A-214"},{"id":407,"day":3,"course":2,"start":"10:15","end":"11:45","room":"B-103"},{"id":408,"day":3,"course":5,"start":"12:30","end":"14:00","room":"A-101"},{"id":409,"day":4,"course":3,"start":"08:30","end":"10:00","room":"C-305"}],
      "tasks":[
        {"id":101,"course":1,"titleLt":"Reikalavimų specifikacija","titleEn":"Requirements specification","due":"2026-11-19","desc":"Prepare a requirements specification for the university portal.","type":"standard"},
        {"id":102,"course":2,"titleLt":"ER diagrama ir SQL schema","titleEn":"ER diagram and SQL schema","due":"2026-11-22","desc":"Design an ER diagram and relational SQL schema.","type":"standard"},
        {"id":103,"course":3,"titleLt":"Žemo tikslumo maketas","titleEn":"Low-fidelity wireframe","due":"2026-11-25","desc":"Create a low-fidelity interface wireframe.","type":"standard"}
      ],
      "materials":[
        {"id":201,"course":1,"title":"Lecture 7 – UML fundamentals","desc":"Class and sequence diagrams"},
        {"id":202,"course":2,"title":"SQL introduction","desc":"SELECT, JOIN, GROUP BY"}
      ],
      "submissions":[
        {"id":301,"task":101,"student":"Laurynas K.","text":"Requirements specification draft","date":"2026-11-08","grade":null,"feedback":""}
      ],"attendance":{}
    })JSON");
}
// -----------------------------------------------------------------------------
// SQLITE STORAGE LAYER
// -----------------------------------------------------------------------------
// SQLite is the only persistent structured-data store. The in-memory Boost.JSON
// objects are used to preserve the existing browser API and simplify the demo.
// SQL tables hold separate assignment, material, submission, schedule, and
// attendance records (rather than storing one JSON document in a database cell).
// Prepared statements safely bind values and avoid SQL injection.
void sqlExec(const char* sql){
    char* error=nullptr;
    if(sqlite3_exec(db,sql,nullptr,nullptr,&error)!=SQLITE_OK){
        std::string msg=error?error:"SQLite error";sqlite3_free(error);
        throw std::runtime_error(msg);
    }
}
struct Stmt{
    sqlite3_stmt* ptr=nullptr;
    explicit Stmt(const char* sql){
        if(sqlite3_prepare_v2(db,sql,-1,&ptr,nullptr)!=SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Stmt(){sqlite3_finalize(ptr);}
    Stmt(const Stmt&)=delete;
    Stmt& operator=(const Stmt&)=delete;
    void reset(){sqlite3_reset(ptr);sqlite3_clear_bindings(ptr);}
    void integer(int i,int64_t v){sqlite3_bind_int64(ptr,i,v);}
    void string(int i,const std::string& v){sqlite3_bind_text(ptr,i,v.c_str(),-1,SQLITE_TRANSIENT);}
    void nullable(int i,const Object& obj,std::string_view key){
        auto it=obj.find(key);
        if(it==obj.end()||it->value().is_null())sqlite3_bind_null(ptr,i);
        else integer(i,num(obj,key));
    }
    void step(){if(sqlite3_step(ptr)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));}
};
// Optional attachment metadata is stored in three columns on the owning record.
void bindFile(Stmt& q,int first,const Object& record){
    auto it=record.find("file");
    if(it==record.end()||!it->value().is_object()){
        for(int n=0;n<3;n++)sqlite3_bind_null(q.ptr,first+n);
        return;
    }
    const auto& file=it->value().as_object();
    q.string(first,str(file,"id"));q.string(first+1,str(file,"name"));
    q.integer(first+2,num(file,"size",0));
}
void addFile(Object& record,sqlite3_stmt* q,int first){
    if(sqlite3_column_type(q,first)==SQLITE_NULL)return;
    record["file"]=Object{
        {"id",std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,first)))},
        {"name",std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,first+1)))},
        {"size",sqlite3_column_int64(q,first+2)}
    };
}
std::string columnText(sqlite3_stmt* q,int i){
    const unsigned char* v=sqlite3_column_text(q,i);
    return v?reinterpret_cast<const char*>(v):"";
}
// Create schema if missing. Foreign keys protect submissions from orphaned tasks.
void initializeDb(){
    if(sqlite3_open((dataDir/"portal.db").string().c_str(),&db)!=SQLITE_OK)
        throw std::runtime_error("Could not open portal.db");
    sqlite3_busy_timeout(db,5000);
    sqlExec("PRAGMA foreign_keys=ON;");
    sqlExec("PRAGMA journal_mode=WAL;");
    sqlExec(R"SQL(
        CREATE TABLE IF NOT EXISTS tasks (
          id INTEGER PRIMARY KEY, course INTEGER NOT NULL,
          title_lt TEXT NOT NULL, title_en TEXT NOT NULL,
          due TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
          type TEXT NOT NULL DEFAULT 'standard',
          file_id TEXT, file_name TEXT, file_size INTEGER
        );
        CREATE TABLE IF NOT EXISTS materials (
          id INTEGER PRIMARY KEY, course INTEGER NOT NULL,
          title TEXT NOT NULL, description TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS submissions (
          id INTEGER PRIMARY KEY, task INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
          student TEXT NOT NULL, answer TEXT NOT NULL DEFAULT '',
          submitted_date TEXT NOT NULL, grade INTEGER, feedback TEXT NOT NULL DEFAULT '',
          file_id TEXT, file_name TEXT, file_size INTEGER,
          UNIQUE(task,student)
        );
        CREATE TABLE IF NOT EXISTS schedule (
          id INTEGER PRIMARY KEY, day INTEGER NOT NULL, course INTEGER NOT NULL,
          start_time TEXT NOT NULL, end_time TEXT NOT NULL, room TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS attendance (
          attendance_key TEXT PRIMARY KEY, present INTEGER NOT NULL CHECK(present IN (0,1))
        );
    )SQL");
}
// Persist changes as one atomic transaction. Small demo datasets make full-table
// synchronization straightforward; a production app would update changed rows only.
void save(){
    sqlExec("BEGIN IMMEDIATE TRANSACTION;");
    try{
        // Delete children first because submissions refer to tasks.
        sqlExec("DELETE FROM submissions; DELETE FROM tasks; DELETE FROM materials;"
                "DELETE FROM schedule; DELETE FROM attendance;");
        const auto& root=state.as_object();
        Stmt tasks("INSERT INTO tasks(id,course,title_lt,title_en,due,description,type,file_id,file_name,file_size) VALUES(?,?,?,?,?,?,?,?,?,?)");
        for(const auto& item:root.at("tasks").as_array()){
            const auto& t=item.as_object();tasks.reset();
            tasks.integer(1,num(t,"id"));tasks.integer(2,num(t,"course"));
            tasks.string(3,str(t,"titleLt"));tasks.string(4,str(t,"titleEn"));
            tasks.string(5,str(t,"due"));tasks.string(6,str(t,"desc"));
            tasks.string(7,str(t,"type","standard"));bindFile(tasks,8,t);tasks.step();
        }
        Stmt materials("INSERT INTO materials(id,course,title,description) VALUES(?,?,?,?)");
        for(const auto& item:root.at("materials").as_array()){
            const auto& m=item.as_object();materials.reset();
            materials.integer(1,num(m,"id"));materials.integer(2,num(m,"course"));
            materials.string(3,str(m,"title"));materials.string(4,str(m,"desc"));materials.step();
        }
        Stmt submissions("INSERT INTO submissions(id,task,student,answer,submitted_date,grade,feedback,file_id,file_name,file_size) VALUES(?,?,?,?,?,?,?,?,?,?)");
        for(const auto& item:root.at("submissions").as_array()){
            const auto& v=item.as_object();submissions.reset();
            submissions.integer(1,num(v,"id"));submissions.integer(2,num(v,"task"));
            submissions.string(3,str(v,"student"));submissions.string(4,str(v,"text"));
            submissions.string(5,str(v,"date"));submissions.nullable(6,v,"grade");
            submissions.string(7,str(v,"feedback"));bindFile(submissions,8,v);submissions.step();
        }
        Stmt schedule("INSERT INTO schedule(id,day,course,start_time,end_time,room) VALUES(?,?,?,?,?,?)");
        for(const auto& item:root.at("schedule").as_array()){
            const auto& v=item.as_object();schedule.reset();
            schedule.integer(1,num(v,"id"));schedule.integer(2,num(v,"day"));
            schedule.integer(3,num(v,"course"));schedule.string(4,str(v,"start"));
            schedule.string(5,str(v,"end"));schedule.string(6,str(v,"room"));schedule.step();
        }
        Stmt attendance("INSERT INTO attendance(attendance_key,present) VALUES(?,?)");
        for(const auto& [key,val]:root.at("attendance").as_object()){
            attendance.reset();attendance.string(1,std::string(key));
            attendance.integer(2,val.as_bool()?1:0);attendance.step();
        }
        sqlExec("COMMIT;");
    }catch(...){sqlExec("ROLLBACK;");throw;}
}
// Rebuild the API-friendly in-memory objects from normalized SQLite rows.
void readDb(){
    Object root{{"tasks",Array{}},{"materials",Array{}},{"submissions",Array{}},
                {"schedule",Array{}},{"attendance",Object{}}};
    {
        Stmt q("SELECT id,course,title_lt,title_en,due,description,type,file_id,file_name,file_size FROM tasks ORDER BY id DESC");
        while(sqlite3_step(q.ptr)==SQLITE_ROW){
            Object x{{"id",sqlite3_column_int64(q.ptr,0)},
                {"course",sqlite3_column_int64(q.ptr,1)},{"titleLt",columnText(q.ptr,2)},
                {"titleEn",columnText(q.ptr,3)},{"due",columnText(q.ptr,4)},
                {"desc",columnText(q.ptr,5)},{"type",columnText(q.ptr,6)}};
            addFile(x,q.ptr,7);root.at("tasks").as_array().push_back(std::move(x));
        }
    }
    {
        Stmt q("SELECT id,course,title,description FROM materials ORDER BY id DESC");
        while(sqlite3_step(q.ptr)==SQLITE_ROW){
            root.at("materials").as_array().push_back(Object{
                {"id",sqlite3_column_int64(q.ptr,0)},
                {"course",sqlite3_column_int64(q.ptr,1)},
                {"title",columnText(q.ptr,2)},{"desc",columnText(q.ptr,3)}});
        }
    }
    {
        Stmt q("SELECT id,task,student,answer,submitted_date,grade,feedback,file_id,file_name,file_size FROM submissions ORDER BY id");
        while(sqlite3_step(q.ptr)==SQLITE_ROW){
            Object x{{"id",sqlite3_column_int64(q.ptr,0)},
                {"task",sqlite3_column_int64(q.ptr,1)},{"student",columnText(q.ptr,2)},
                {"text",columnText(q.ptr,3)},{"date",columnText(q.ptr,4)},
                {"grade",sqlite3_column_type(q.ptr,5)==SQLITE_NULL?json::value(nullptr):json::value(sqlite3_column_int64(q.ptr,5))},
                {"feedback",columnText(q.ptr,6)}};
            addFile(x,q.ptr,7);root.at("submissions").as_array().push_back(std::move(x));
        }
    }
    {
        Stmt q("SELECT id,day,course,start_time,end_time,room FROM schedule ORDER BY id");
        while(sqlite3_step(q.ptr)==SQLITE_ROW){
            root.at("schedule").as_array().push_back(Object{
                {"id",sqlite3_column_int64(q.ptr,0)},{"day",sqlite3_column_int64(q.ptr,1)},
                {"course",sqlite3_column_int64(q.ptr,2)},{"start",columnText(q.ptr,3)},
                {"end",columnText(q.ptr,4)},{"room",columnText(q.ptr,5)}});
        }
    }
    {
        Stmt q("SELECT attendance_key,present FROM attendance");
        while(sqlite3_step(q.ptr)==SQLITE_ROW)
            root.at("attendance").as_object()[columnText(q.ptr,0)]=sqlite3_column_int(q.ptr,1)!=0;
    }
    state=std::move(root);
}
// Migrate data.json once if a previous installation is present; otherwise seed
// new demo data. The old JSON file is renamed for backup, never written again.
void load(){
    initializeDb();
    Stmt count("SELECT (SELECT COUNT(*) FROM tasks)+(SELECT COUNT(*) FROM materials)+(SELECT COUNT(*) FROM submissions)+(SELECT COUNT(*) FROM schedule)+(SELECT COUNT(*) FROM attendance)");
    bool empty=sqlite3_step(count.ptr)==SQLITE_ROW && sqlite3_column_int64(count.ptr,0)==0;
    if(empty){
        auto old=dataDir/"data.json";
        if(std::filesystem::exists(old)){
            std::ifstream in(old,std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(in)),{});
            state=json::parse(s);
            if(!state.as_object().contains("schedule"))
                state.as_object()["schedule"]=seed().as_object().at("schedule");
        }else state=seed();
        save();
        if(std::filesystem::exists(old)){
            auto backup=dataDir/"data.json.migrated-backup";
            if(!std::filesystem::exists(backup))std::filesystem::rename(old,backup);
        }
    }
    readDb();
    for(auto key:{"tasks","materials","submissions","schedule"})
        for(auto& item:state.as_object().at(key).as_array())
            nextId=std::max(nextId,static_cast<uint64_t>(num(item.as_object(),"id",0)+1));
}
// Return the shared state, hiding other students’ submissions and attendance from student accounts.
json::value publicState(const std::string& role){
    if(role=="teacher")return state;
    auto copy=state;Array filtered;
    for(auto& s:state.as_object().at("submissions").as_array())
        if(str(s.as_object(),"student")=="Laurynas K.")filtered.push_back(s);
    copy.as_object()["submissions"]=std::move(filtered);
    copy.as_object()["attendance"]=Object{};
    return copy;
}
// Build a standard success/error object returned by mutation operations.
Object result(bool ok,std::string msg){return Object{{"ok",ok},{"message",msg}};}
// Course Service: course details and student roster (teacher only).
// Logical Course Service: provide a course, its tasks/materials and the teacher-only roster.
json::value courseDetails(int64_t id,const std::string& role){
    if(id<1||id>6||(role=="teacher"&&!validCourse(id)))
        return Object{{"ok",false},{"message","Course unavailable"}};
    const char* codes[]={"","PSIN","DB","ZKS","","MA","ANG"};
    Object result{{"ok",true},{"id",id},{"code",codes[id]}};
    Array tasks,materials;
    for(const auto& t:state.as_object().at("tasks").as_array())
        if(num(t.as_object(),"course")==id)tasks.push_back(t);
    for(const auto& m:state.as_object().at("materials").as_array())
        if(num(m.as_object(),"course")==id)materials.push_back(m);
    result["assignments"]=std::move(tasks);
    result["materials"]=std::move(materials);
    if(role=="teacher")result["students"]=Array{"Laurynas K.","Austėja Petrauskaitė","Mantas Jankauskas","Gabija Kazlauskaitė","Lukas Žukauskas","Emilija Vaitkutė"};
    return result;
}
// Assignment Service: teacher can retrieve submissions by course.
// Logical Assignment Service: return submissions for one teacher-owned course.
json::value courseSubmissions(int64_t course,const std::string& role){
    if(role!="teacher"||!validCourse(course))return Object{{"ok",false},{"message","Not permitted"}};
    Array items;
    const auto& tasks=state.as_object().at("tasks").as_array();
    for(const auto& sub:state.as_object().at("submissions").as_array()){
        int64_t taskId=num(sub.as_object(),"task");
        for(const auto& t:tasks)if(num(t.as_object(),"id")==taskId&&num(t.as_object(),"course")==course){
            items.push_back(sub);break;
        }
    }
    return Object{{"ok",true},{"submissions",std::move(items)}};
}

// Central write handler. Each authorized action updates memory then commits to SQLite.
Object mutate(const std::string& role,const Object& req){
    auto op=str(req,"action");
    auto& root=state.as_object();
    auto& tasks=root.at("tasks").as_array();
    auto& mats=root.at("materials").as_array();
    auto& subs=root.at("submissions").as_array();
    auto findId=[](Array& a,int64_t id)->Object*{
        for(auto& x:a)if(num(x.as_object(),"id")==id)return &x.as_object();
        return nullptr;
    };
    auto taskCourse=[&](int64_t id)->int64_t{auto* t=findId(tasks,id);return t?num(*t,"course"):-1;};
    auto teacherOnly=[&](){return role=="teacher";};
    // TEACHER: create an assignment; an optional file is stored separately.
    if(op=="createTask"&&teacherOnly()){
        auto c=num(req,"course");auto title=str(req,"title");auto due=str(req,"due");
        if(!validCourse(c)||title.empty()||title.size()>180||due.size()!=10)return result(false,"Invalid assignment");
        Object newTask{{"id",nextId++},{"course",c},{"titleLt",title},{"titleEn",title},{"due",due},{"desc",str(req,"desc")},{"type","standard"}};
        if(auto it=req.find("file");it!=req.end()&&!it->value().is_null())newTask["file"]=saveAttachment(it->value());
        tasks.insert(tasks.begin(),std::move(newTask));
    // TEACHER: deleting an assignment also removes its submission records.
    }else if(op=="removeTask"&&teacherOnly()){
        auto id=num(req,"id");if(!findId(tasks,id))return result(false,"Assignment not found");
        tasks.erase(std::remove_if(tasks.begin(),tasks.end(),[&](const auto& x){return num(x.as_object(),"id")==id;}),tasks.end());
        subs.erase(std::remove_if(subs.begin(),subs.end(),[&](const auto& x){return num(x.as_object(),"task")==id;}),subs.end());
    // TEACHER: add a course material record.
    }else if(op=="addMaterial"&&teacherOnly()){
        auto c=num(req,"course");auto title=str(req,"title");
        if(!validCourse(c)||title.empty()||title.size()>180)return result(false,"Invalid material");
        mats.insert(mats.begin(),Object{{"id",nextId++},{"course",c},{"title",title},{"desc",str(req,"desc")}});
    // TEACHER: remove a course material record.
    }else if(op=="removeMaterial"&&teacherOnly()){
        auto id=num(req,"id");if(!findId(mats,id))return result(false,"Material not found");
        mats.erase(std::remove_if(mats.begin(),mats.end(),[&](const auto& x){return num(x.as_object(),"id")==id;}),mats.end());
    // STUDENT: submit or replace an answer; a new submission resets its previous grade.
    }else if(op=="submit"&&role=="student"){
        auto task=num(req,"task");auto answer=str(req,"text");
        if(!validCourse(taskCourse(task))||answer.size()>30000)return result(false,"Invalid submission");
        auto fileIt=req.find("file");bool hasFile=fileIt!=req.end()&&!fileIt->value().is_null();
        Object* existing=nullptr;
        for(auto& s:subs)if(num(s.as_object(),"task")==task&&str(s.as_object(),"student")=="Laurynas K.")existing=&s.as_object();
        if(answer.empty()&&!hasFile&&!(existing&&existing->contains("file")))return result(false,"Answer or file required");
        Object attachment;
        if(hasFile)attachment=saveAttachment(fileIt->value());
        if(existing){(*existing)["text"]=answer;(*existing)["date"]=nowDate();(*existing)["grade"]=nullptr;(*existing)["feedback"]="";
            if(hasFile)(*existing)["file"]=std::move(attachment);
        }else{
            Object sub{{"id",nextId++},{"task",task},{"student","Laurynas K."},{"text",answer},{"date",nowDate()},{"grade",nullptr},{"feedback",""}};
            if(hasFile)sub["file"]=std::move(attachment);
            subs.push_back(std::move(sub));
        }
    // TEACHER: grade an existing submission on a 1–10 scale and add feedback.
    }else if(op=="grade"&&teacherOnly()){
        auto* s=findId(subs,num(req,"id"));auto grade=num(req,"grade");
        if(!s||grade<1||grade>10||!validCourse(taskCourse(num(*s,"task"))))return result(false,"Invalid grade");
        (*s)["grade"]=grade;(*s)["feedback"]=str(req,"feedback");
    // TEACHER: add/edit a weekday class; reject invalid times and overlapping classes.
    }else if((op=="addSchedule"||op=="updateSchedule")&&teacherOnly()){
        auto course=num(req,"course"),day=num(req,"day");
        auto start=str(req,"start"),end=str(req,"end"),room=str(req,"room");
        auto validTime=[](const std::string& s){
            if(s.size()!=5||s[2]!=':'||!std::isdigit(static_cast<unsigned char>(s[0]))||
                !std::isdigit(static_cast<unsigned char>(s[1]))||!std::isdigit(static_cast<unsigned char>(s[3]))||
                !std::isdigit(static_cast<unsigned char>(s[4])))return false;
            int h=std::stoi(s.substr(0,2)),m=std::stoi(s.substr(3,2));return h<24&&m<60;
        };
        if(!validCourse(course)||day<0||day>4||!validTime(start)||!validTime(end)||
           start>=end||room.empty()||room.size()>50)return result(false,"Invalid schedule entry");
        auto& schedule=root.at("schedule").as_array();
        Object* current=op=="updateSchedule"?findId(schedule,num(req,"id")):nullptr;
        if(op=="updateSchedule"&&(!current||!validCourse(num(*current,"course"))))
            return result(false,"Schedule entry unavailable");
        // A teacher cannot book two of their classes at the same time.
        for(const auto& e:schedule){
            const auto& slot=e.as_object();
            if(current&&num(slot,"id")==num(*current,"id"))continue;
            if(validCourse(num(slot,"course"))&&num(slot,"day")==day&&
               start<str(slot,"end")&&end>str(slot,"start"))
                return result(false,"This time overlaps another teacher class");
        }
        if(current){(*current)["course"]=course;(*current)["day"]=day;
            (*current)["start"]=start;(*current)["end"]=end;(*current)["room"]=room;
        }else schedule.push_back(Object{{"id",nextId++},{"course",course},{"day",day},
            {"start",start},{"end",end},{"room",room}});
    // TEACHER: delete a schedule entry for one of the teacher’s courses.
    }else if(op=="removeSchedule"&&teacherOnly()){
        auto& schedule=root.at("schedule").as_array();
        auto id=num(req,"id");auto* slot=findId(schedule,id);
        if(!slot||!validCourse(num(*slot,"course")))return result(false,"Schedule entry unavailable");
        schedule.erase(std::remove_if(schedule.begin(),schedule.end(),[&](const auto& x){
            return num(x.as_object(),"id")==id;}),schedule.end());
    // TEACHER: record attendance flags; save() commits them to SQLite.
    }else if(op=="attendance"&&teacherOnly()){
        auto it=req.find("values");if(it==req.end()||!it->value().is_object())return result(false,"Invalid attendance");
        for(auto& [k,v]:it->value().as_object()){
            if(!v.is_bool()||k.size()>64)return result(false,"Invalid attendance entry");
            root.at("attendance").as_object()[k]=v;
        }
    }else return result(false,"Action not permitted for this role");
    save();return result(true,"Saved");
}
using Request=http::request<http::string_body>;
using Response=http::response<http::string_body>;
// Wrap a JSON result in an HTTP response with content type and no-store caching.
Response response(http::status status,json::value payload){
    Response r{status,11};r.set(http::field::content_type,"application/json; charset=utf-8");
    r.set(http::field::cache_control,"no-store");r.body()=json::serialize(payload);r.prepare_payload();return r;
}
// Normalize the request target before routing. Proxies and browsers can add query
// strings (e.g. /?source=render), and HTTP also permits absolute-form URLs.
// Route matching must use only the pathname, not the full target string.
std::string routePath(const Request& req){
    std::string path(req.target());
    if(path.rfind("http://",0)==0 || path.rfind("https://",0)==0){
        const auto schemeEnd=path.find("://")+3;
        const auto firstSlash=path.find('/',schemeEnd);
        path=firstSlash==std::string::npos?"/":path.substr(firstSlash);
    }
    const auto query=path.find_first_of("?#");
    if(query!=std::string::npos)path.resize(query);
    if(path.empty())path="/";
    return path;
}
// HTTP router: serve the frontend, authenticate, check sessions and dispatch API requests.
Response handle(const Request& req){
    auto path=routePath(req);
    // Serve the HTML frontend through the same local server as the API.
    if(req.method()==http::verb::get&&(path=="/"||path=="/index.html")){
        std::ifstream in(base/"index.html",std::ios::binary);
        if(!in)return response(http::status::not_found,result(false,"index.html not found"));
        Response r{http::status::ok,11};r.set(http::field::content_type,"text/html; charset=utf-8");
        r.set(http::field::cache_control,"no-store");r.body()=std::string((std::istreambuf_iterator<char>(in)),{});r.prepare_payload();return r;
    }
    if(req.method()==http::verb::get&&path=="/api/health")return response(http::status::ok,Object{{"ok",true},{"backend","C++ / Boost.Beast"}});
    // A missing website page should never be confused with a login failure.
    // Only /api/* endpoints need a session; / and /index.html remain public.
    if(path.rfind("/api/",0)!=0)
        return response(http::status::not_found,result(false,"Unknown website path: "+path));
    try{
        std::lock_guard<std::mutex> lock(guard);
        // Authenticate a demo account and return the server-determined teacher/student role.
        if(req.method()==http::verb::post&&path=="/api/login"){
            auto j=json::parse(req.body());if(!j.is_object())return response(http::status::bad_request,result(false,"Invalid request"));
            const auto& obj=j.as_object();auto email=str(obj,"email"),password=str(obj,"password");
            // Determine the role server-side from the authenticated demo account.
            // Never trust a role sent by the browser.
            std::transform(email.begin(),email.end(),email.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            std::string role;
            if(password=="demo123" && email=="destytojas@mruni.eu")role="teacher";
            else if(password=="demo123" && email=="studentas@stud.mruni.eu")role="student";
            else return response(http::status::unauthorized,result(false,"Incorrect demo credentials"));
            auto token=randomToken();sessions[token]=role;
            return response(http::status::ok,Object{{"ok",true},{"token",token},{"role",role}});
        }
        // All routes below require the Bearer token created at login.
        auto auth=std::string(req[http::field::authorization]);
        if(auth.rfind("Bearer ",0)!=0||!sessions.count(auth.substr(7)))return response(http::status::unauthorized,result(false,"Please sign in"));
        auto token=auth.substr(7),role=sessions.at(token);
        if(req.method()==http::verb::post&&path=="/api/logout"){
            sessions.erase(token);return response(http::status::ok,result(true,"Logged out"));
        }
        // Return the role-filtered state for dashboard, assignments and schedule.
        if(req.method()==http::verb::get&&path=="/api/state")return response(http::status::ok,publicState(role));
        // Stream a saved attachment only if this account may access it.
        if(req.method()==http::verb::get&&path.rfind("/api/files/",0)==0){
            auto id=path.substr(11);
            if(id.empty()||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.")!=std::string::npos||!canDownload(id,role))
                return response(http::status::forbidden,result(false,"Attachment unavailable"));
            std::ifstream in(dataDir/"uploads"/id,std::ios::binary);
            if(!in)return response(http::status::not_found,result(false,"Attachment not found"));
            Response r{http::status::ok,11};
            r.set(http::field::content_type,"application/octet-stream");
            r.set(http::field::content_disposition,"attachment");
            r.set(http::field::cache_control,"no-store");
            r.body()=std::string((std::istreambuf_iterator<char>(in)),{});
            r.prepare_payload();return r;
        }
        // Sequence diagram: Portal -> Course Service -> Database -> Portal.
        if(req.method()==http::verb::get&&path.rfind("/api/courses/",0)==0){
            const auto suffix=path.substr(std::string("/api/courses/").size());
            auto split=suffix.find('/');
            auto number=split==std::string::npos?suffix:suffix.substr(0,split);
            if(number.empty()||number.find_first_not_of("0123456789")!=std::string::npos)
                return response(http::status::bad_request,result(false,"Invalid course"));
            int64_t course=std::stoll(number);
            if(split==std::string::npos){
                auto details=courseDetails(course,role);
                const bool ok=details.as_object().at("ok").as_bool();
                return response(ok?http::status::ok:http::status::forbidden,std::move(details));
            }
            if(suffix.substr(split)=="/submissions"){
                auto list=courseSubmissions(course,role);
                const bool ok=list.as_object().at("ok").as_bool();
                return response(ok?http::status::ok:http::status::forbidden,std::move(list));
            }
        }
        // Sequence diagram: Portal -> Assignment Service -> Database -> Portal.
        if(req.method()==http::verb::post&&(path=="/api/assignments"||path=="/api/grades")){
            auto j=json::parse(req.body());if(!j.is_object())return response(http::status::bad_request,result(false,"Invalid request"));
            auto payload=j.as_object();payload["action"]=path=="/api/assignments"?"createTask":"grade";
            auto out=mutate(role,payload);
            const bool ok=out.at("ok").as_bool();
            return response(ok?http::status::ok:http::status::bad_request,std::move(out));
        }

        // Generic actions: student submissions, attendance, materials and schedule edits.
        if(req.method()==http::verb::post&&path=="/api/action"){
            auto j=json::parse(req.body());if(!j.is_object())return response(http::status::bad_request,result(false,"Invalid request"));
            auto resultObj=mutate(role,j.as_object());
            const bool ok=resultObj.at("ok").as_bool();
            return response(ok?http::status::ok:http::status::bad_request,std::move(resultObj));
        }
    }catch(const std::exception& e){return response(http::status::bad_request,result(false,e.what()));}
    return response(http::status::not_found,result(false,"Not found"));
}
// Start the local-only HTTP server; read one request per connection and return its response.
int main(int argc,char** argv){
    try{
        base=std::filesystem::absolute(argc>1?argv[1]:".");
        // When MRU_DATA_DIR is set, save SQLite and uploads outside the app folder.
        // On Render paid plans, point this to the persistent disk (e.g. /var/data).
        const char* dirEnv=std::getenv("MRU_DATA_DIR");
        dataDir=dirEnv&&*dirEnv?std::filesystem::absolute(dirEnv):base;
        std::filesystem::create_directories(dataDir);
        // Cloud platforms pass the external port via PORT; local scripts still pass argv[2].
        const char* portEnv=std::getenv("PORT");
        int port=argc>2?std::stoi(argv[2]):(portEnv&&*portEnv?std::stoi(portEnv):8080);
        if(port<1||port>65535)throw std::runtime_error("Invalid port");
        load();asio::io_context ioc;
        // 127.0.0.1 is safer locally; Docker sets MRU_HOST=0.0.0.0 for public routing.
        const char* hostEnv=std::getenv("MRU_HOST");
        std::string host=hostEnv&&*hostEnv?hostEnv:"127.0.0.1";
        tcp::acceptor acceptor(ioc,{asio::ip::make_address(host),static_cast<unsigned short>(port)});
        std::cout<<"MRU Portal C++ / SQLite server running at http://"<<host<<":"<<port<<"\n"<<std::flush;
        for(;;){
            tcp::socket socket(ioc);acceptor.accept(socket);
            try{
                beast::flat_buffer buffer;
                http::request_parser<http::string_body> parser;
                parser.body_limit(9*1024*1024);
                // Some cloud load balancers and browsers open a TCP connection
                // then close it without sending an HTTP request. Boost.Beast
                // reports this as end_of_stream: it is not an application error.
                beast::error_code readError;
                http::read(socket,buffer,parser,readError);
                if(readError==http::error::end_of_stream || readError==asio::error::eof){
                    // Expected disconnect before a request; do not flood Render logs.
                    continue;
                }
                if(readError){
                    // Keep genuine HTTP parsing / network failures visible.
                    std::cerr<<"Request read error: "<<readError.message()<<"\n";
                    continue;
                }
                Request req=parser.release();
                auto res=handle(req);res.keep_alive(false);
                // Diagnostic: log method, exact incoming target, normalized path,
                // and HTTP status. Do NOT log authorization headers or passwords.
                std::cout<<"HTTP "<<req.method_string()<<" "<<req.target()
                         <<" [path="<<routePath(req)<<"] -> "<<res.result_int()<<"\n"<<std::flush;
                http::write(socket,res);
                beast::error_code ec;socket.shutdown(tcp::socket::shutdown_both,ec);
            }catch(const std::exception& e){std::cerr<<"Request error: "<<e.what()<<"\n";}
        }
    }catch(const std::exception& e){std::cerr<<"Server error: "<<e.what()<<"\n";return 1;}
}
