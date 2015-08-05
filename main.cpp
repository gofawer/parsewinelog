/*
The MIT License (MIT)

Copyright (c) 2015 Philippe Groarke

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

std::mutex mutex;
std::condition_variable processCondition;

std::ifstream::pos_type fileSize;

std::string help = "Usage: parsewinelog [yourlog.txt]";

std::ifstream::pos_type getFilesize(const std::string& filename)
{
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

static inline void progressBar(unsigned int x, unsigned int n, unsigned int w = 50)
{
    if ( (x != n) && (x % (n/100+1) != 0) ) return;

    float ratio  =  x/(float)n;
    int   c      =  ratio * w;

    std::cout << std::setw(3) << (int)(ratio*100) << "% [";
    for (int x=0; x<c; x++) std::cout << "=";
    for (int x=c; x<w; x++) std::cout << " ";
    std::cout << "]\r" << std::flush;
}

std::ifstream openInFile(std::string f)
{
        std::ifstream inFile(f, std::ios::in);

        if (!inFile.is_open()) {
                std::cout << "Couldn't read input file: " << f << std::endl;
                inFile = std::ifstream();
        } else {
                fileSize = getFilesize(f);
                std::cout << "Parsing: " << f
                        << " -- Filesize: " << fileSize << std::endl;
        }

        return inFile;
}

std::ofstream openOutFile(std::string f)
{
        auto extPos = f.find_last_of(".");
        std::string extension = f.substr(extPos);
        std::string outFilename = f.substr(0, extPos);
        f = outFilename + "_parsed" + extension;
        std::ofstream outFile(f, std::ios::out);

        if (!outFile.is_open()) {
                std::cout << "Couldn't create output file: " << f << std::endl;
                outFile = std::ofstream();
        }

        return outFile;
}


struct Parser {
        Parser(std::string call) : Call(call) {}

        bool operator()(const std::string& Ret) const
        {
                // Check the call match.
                auto callPosBegin = Call.find("Call ") + 5;
                auto callPosEnd = Call.find_first_of('(') - callPosBegin;
                std::string funcCall = Call.substr(callPosBegin, callPosEnd);
                // std::cout << funcCall << std::endl;

                if (Ret.find(funcCall) == std::string::npos)
                return false;

                // Check return address, if it exists.
                auto addrPosBegin = Call.find("ret=");

                if (addrPosBegin == std::string::npos)
                return true;

                addrPosBegin += 4;
                std::string addr = Call.substr(addrPosBegin);
                // std::cout << addr << std::endl;

                if (Ret.find(addr) == std::string::npos)
                return false;

                // std::cout << std::endl << "Match" << std::endl;
                // std::cout << Call << std::endl << "    - " << Ret << std::endl;
                return true;
        }

        std::string Call;
};

struct Thread {
        Thread() { myThread = std::thread(&Thread::run, this); }

        ~Thread()
        {
                running = false;
                myThread.join();
        }

        friend std::ostream& operator<<(std::ostream& os, Thread& t)
        {
                std::lock_guard<std::mutex> lk(t.vectorMutex);
                os << t.parseVector[0].Call << std::endl;
                t.parseVector.erase(t.parseVector.begin());
                return os;
        }

        void run()
        {
                while(running) {
                        // Wait to be notified.
                        std::unique_lock<std::mutex> lock(mutex);
                        processCondition.wait(lock);

                        std::lock_guard<std::mutex> lk(vectorMutex);
                        for (int i = 0; i < parseVector.size(); ++i) {
                                if (parseVector[i](currentWork)) {
                                        parseVector.erase(parseVector.begin() + i);
                                        // std::cout << "erase" << std::endl;
                                        break;
                                }
                        }
                }
        }

        void stop()
        {
                running = false;
        }

        void addCall(std::string call)
        {
                std::lock_guard<std::mutex> lk(vectorMutex);
                parseVector.push_back(Parser(std::move(call)));
        }

        void processLine(std::string line)
        {
                currentWork = line;
        }

        int size()
        {
                return parseVector.size();
        }

        bool running = true;
        std::string currentWork;
        std::thread myThread;
        std::mutex vectorMutex;
        std::vector<Parser> parseVector;
};

struct ThreadPool {
        ThreadPool()
        {
                auto numThreads = std::thread::hardware_concurrency() - 1;
                if (numThreads <= 0)
                numThreads = 1;
                for (auto i = 0; i < numThreads; ++i) {
                        pool.emplace_back(new Thread());
                }
        }

        friend std::ostream& operator<<(std::ostream& os, ThreadPool& tp)
        {
                int i = 0;
                while (!tp.pool.empty()) {
                        if (tp.pool[i]->size() == 0) {
                                i = tp.killThread(i);
                                continue;
                        }

                        os << *(tp.pool[i]);
                        i = (i + 1) % tp.pool.size();
                }
                return os;
        }

        int killThread(const int& i)
        {
                pool[i]->stop();
                processCondition.notify_all();
                pool.erase(pool.begin() + i);
                return i < pool.size() ? i : 0;
        }

        void killAll()
        {
                for (const auto& x : pool) {
                        x->stop();
                }
                processCondition.notify_all();
        }

        void enqueue(std::string call)
        {
                auto next = std::min_element(pool.begin(), pool.end(),
                [](const std::unique_ptr<Thread>& i,
                        const std::unique_ptr<Thread>& j)
                        { return i->size() < j->size(); });

                        (**next).addCall(std::move(call));
                }

                void process(std::string line)
                {
                        std::lock_guard<std::mutex> lock(mutex);
                        for (const auto& x : pool) {
                                x->processLine(line);
                        }
                        processCondition.notify_all();
                }

                int size()
                {
                        int ret = 0;
                        for (const auto& x : pool) {
                                ret += x->size();
                        }
                        return ret;
                }

                std::vector<std::unique_ptr<Thread>> pool;
        };

        int main(int argc, char** argv)
        {
                // Print Help //
                if (argc <= 1 || argc > 2) {
                        std::cout << help << std::endl;
                        return 0;
                }

                // Initialize //
                std::string filename = std::string(argv[1]);
                std::ifstream inFile = openInFile(filename);
                std::ofstream outFile = openOutFile(filename);
                ThreadPool workerPool;


                // Parse //
                int lineNum = 0;
                std::string line;
                while (getline(inFile, line)) {
                        // std::cout << "Parsing line: " << ++lineNum << std::endl;
                        if (line.find("Call") != std::string::npos) {
                                workerPool.enqueue(std::move(line));
                        } else if (line.find("Ret") != std::string::npos) {
                                workerPool.process(std::move(line));
                        } else {
                                outFile << line << std::endl;
                        }
                        progressBar((inFile.tellg()/100) + 1, fileSize/100);
                }

                // Finalise //
                std::cout << std::endl << "Lines left: " << workerPool.size()
                        << " -- Outputting to file." << std::endl;
                // std::cout << workerPool;
                outFile << workerPool;
                // Cleanup //
                inFile.close();
                outFile.close();
                return 0;
        }
