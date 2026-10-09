// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include<sstream>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdio>
#include <stdexcept>
#include "MyStack.h"

using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;

        Node(const T& val) {
            data = val;
            next = nullptr;
        }
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count == MAX_STACK_DEPTH) {
            return;
        }
        Node* n = new Node(val);

        if (isEmpty()) {
            top = n;
        }
        else {
            n->next = top;
            top = n;
        }
        count++;
    }
    T pop()
    {

        if (isEmpty()) {
            throw underflow_error("Stack is empty.");
        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        
        count--;
        return val;
    }
    T& peek()
    {
        if (isEmpty()) {
            throw underflow_error("Stack is empty.");
        }
        return top->data;

    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t c = count;
        int i = 0;
        Node* temp = top;
        while (temp!=nullptr && i < maxLen) {
            out[i] = temp->data;
            temp = temp->next;
            i++;
        }

        return c;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;

    TimelineNode(Snapshot* s)
    {
        data = s;
        next = nullptr;
        prev = nullptr;
    }
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        stepCount = 0;
        head = nullptr;
        tail = nullptr;
    }
    void record(Snapshot* s)
    {
        TimelineNode* t = new TimelineNode(s);

        if (stepCount == 0) {
            head = t;
            tail = head;
        }
        else{
            tail->next = t;
            t->prev = tail;
            tail = t;

        }
        stepCount++;
    }
    TimelineNode* begin()
    {
    
        return head;
    }
    int32_t getStepCount()
    {

        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    Variable* argvRef[MAX_VARS_PER_FRAME];

    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits

    // FuncEntry(string n, int64_t b) : funcName(n), byteOffsetInResolveBin(b){};
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;

    // PendingPatch(int64_t b, string t): byteOffsetOfOffsetField(b), targetFuncName(t){};
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line
     while (getline(in, out)){
        stringstream ss(out);
        string temp;

        if (ss >> temp)
            return true;
    }

    return false;
}
string firstWord(const string& line)
{
    // returns first word from the input string
    string first;
    stringstream ss(line);

    ss >> first;

    return first;
}
string secondWord(const string& line)
{
    // returns the second word
    string first, sec;
    stringstream ss(line);

    ss >> first >> sec;

    return sec;
}
bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream in(sourcePath, ios::binary);

    if (!in)
        return false;

    string line;

    MyStack<string> st;

    while (readSourceLine(in, line)){
        if(firstWord(line) == "func"){
            if(secondWord(line) == ""){
                return false;
            }
            if(st.isEmpty()){
                st.push("func");
            }
            else{
                return false;
            }
        }
        else if(firstWord(line) == "func_end"){
            if(st.isEmpty()){
                return false;
            }
            if(st.top() == "func"){
                st.pop();
            }
            else{
                return false;
            }
        }
    }

    if(!st.isEmpty()){
        return false;
    }

    return true;

}

int64_t findFunctionOffset(FuncEntry funcArray[], int32_t funcCount, const string& target){
    for (int32_t i = 0; i < funcCount; i++){
        if (funcArray[i].funcName == target){
            return funcArray[i].byteOffsetInResolveBin;

        }
    }

    return -1;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int32_t s_size = text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&s_size, sizeof(int32_t), 1, f);
    fwrite(text.data(), sizeof(char),  s_size, f);

    // return offsetField+ sizeof(int64_t) + sizeof(int32_t) + s_size;
    return offsetField;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offset;
    int32_t size;

     if (fread(&offset, sizeof(int64_t), 1, f) != 1)
        return -1;

    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return -1;

    if (size < 0)
        return -1;

    outText.resize(size);
    if (size > 0 && fread(&outText[0], sizeof(char), size, f) != static_cast<size_t>(size))
        return -1;

    return offset;

}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 

    FILE* f = fopen(resolveBinPath, "wb");
    if(!f){
        return -1;
    }

    ifstream in(sourcePath, ios::binary);

    if (!in)
        return -1;

    string line;

    int64_t offset = 0;

    while (readSourceLine(in, line)){

        // int64_t curOffset = nextOffset;
        offset = writeResolveRecord(f, offset, line);


        if(firstWord(line) == "func" && funcCount < MAX_FUNCS){
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = offset;
            funcCount++;
        }

        if(firstWord(line) == "call" && patchCount  < MAX_PATCHES){
            patches[patchCount].targetFuncName = secondWord(line);
            patches[patchCount].byteOffsetOfOffsetField = offset;
            patchCount++;
        }

        offset = offset+ sizeof(int64_t) + sizeof(int32_t) + line.size();

    }

    for(int32_t i = 0; i<patchCount; i++){
        int64_t targetOffset = findFunctionOffset(funcArray, funcCount, patches[i].targetFuncName);
        if(targetOffset == -1){
            fclose(f);

            return -1;
        }
        fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, f);
        fseek(f, 0, SEEK_END);

    }

    fclose(f);

    int64_t mainOffset = findFunctionOffset(funcArray, funcCount, "main");

    if(mainOffset == -1){
        return -1;
    }
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
    // Token(TokenType t, string txt) : type(t), text(txt) {}
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    stringstream ss(line);
    string word;

    int32_t ct = 0;
    while (ss >> word && ct < maxTokens){
        Token token;

        if(ct == 0){
            token.type = KEYWORD;
        }
        else if(ct == 1){
            token.type = IDENTIFIER;
        }
        else{
            token.type = PARAM;
        }
        token.text = word;
        
        tokens[ct++] = token;
    
    }

    return ct;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}

Variable* findVar(Frame& frame, const string& name){

    for (int32_t i = 0; i < frame.argc; i++){
        if (frame.argv[i].name == name){
            if (frame.argvRef[i] != nullptr){
                return frame.argvRef[i];
            }

            return &frame.argv[i];
        }
    }

    for (int32_t i = 0; i < frame.localCount; i++){
        if (frame.locals[i].name == name){
            return &frame.locals[i];
        }
    }

    return nullptr;

}

void setVar(Frame& frame, const string& name, int32_t value){

    Variable* v = findVar(frame, name);

    if(v == nullptr){
        if(frame.localCount < MAX_VARS_PER_FRAME){
                Variable var;
                var.name = name;
                var.value = value;
                frame.locals[frame.localCount++] = var;
            }
    }
    else{
        v->value = value;
    }

}


bool getVarValueOrLit(Frame& frame, const string& word, int32_t& value){
    Variable* v = findVar(frame, word);

    if (v != nullptr) {
        value = v->value;
        return true;
    }
    try{
        value = stoi(word);
        return true;
    }
    catch (const invalid_argument& e){
        return false;
    }

}

bool doArithmetic(Frame& frame, Token tokens[], int32_t tCount){

    if(tCount < 3){
        // cout << "Invalid args";

        return false;
    }

    string op = tokens[0].text;

    Variable * destVar = findVar(frame, tokens[1].text);
    if(destVar == nullptr){
        return false;
    }

    int32_t result = destVar->value;

    for(int i = 2; i < tCount;i++){
        int32_t operand;
        if(!getVarValueOrLit(frame, tokens[i].text, operand)){
            // cout << "Invalid args";
            return false;
        }

        if(op == "add"){
            result += operand;
        }
        else if(op == "sub"){
            result -= operand;
        }
        else if(op == "mul"){
            result *= operand;
        }
        else if(op == "div"){
            if(operand == 0){
                // cout << "err :div by zero";
                return false;
            }
            result /= operand;
        }

        destVar->value = result;
    }

    return true;
    

}


void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack
    // instruction set = [func, func_end, call, set, add, sub, mul and div]

    // implementation:
    // execute line by line, and according to the keyword perform action

    FILE* f = fopen(resolveBinPath, "rb");

    if (!f)
        return;

    fseek(f, mainOffset, SEEK_SET);

    Stack<Frame> callStack;
    Frame mainFrame{};
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;

    callStack.push(mainFrame);

    while (true){
        string line;

        int64_t lineOffset = readResolveRecord(f, line);
        if(lineOffset == -1){
            break;
        }

        Token tokens[MAX_TOKENS];
        int32_t tCount = tokenizeLine(line, tokens, MAX_TOKENS);

        if (tCount == 0){
            continue;

        }

        if(tokens[0].text == "call"){
                int64_t nextLine = ftell(f);

                if(tCount<2){
                    cout<<"invalid func call";
                    break;
                }

                bool valid = true;


                Frame func{};
                func.func_name = tokens[1].text;
                func.argc = 0;
                func.localCount = 0;
                func.returnLine = nextLine;

                Frame& curFrame = callStack.peek();

                for(int i = 2; i  < tCount; i++){
                    Variable* var = findVar(curFrame, tokens[i].text);
                    if(var == nullptr){
                        // cout << "Invalid arg";
                        valid = false;
                        break;
                    }
                    func.argv[func.argc].name = tokens[i].text;
                    func.argv[func.argc].value = var->value;
                    func.argvRef[func.argc++] = var;
                }

                if(!valid){
                    break;
                }

                callStack.push(func);

        }
        else if(tokens[0].text == "set"){

            if (tCount != 3) {
                // cout << "Invalid syntax for set";

                break; 
            }

            Frame& curFunc = callStack.peek();

            string destVar = tokens[1].text;
            string valToSet = tokens[2].text;

            int32_t value;

            if (!getVarValueOrLit(curFunc, valToSet, value)) {
                // cout << "Invalid argument";
                break;
            }

            setVar(curFunc, destVar, value);
        }
        else if(tokens[0].text == "add" || tokens[0].text == "sub" 
            || tokens[0].text ==  "mul" || tokens[0].text ==  "div"){

                Frame& curFunc = callStack.peek();

                if(!doArithmetic(curFunc, tokens, tCount)){
                    break;
                }

        }




        
    


    }
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}
