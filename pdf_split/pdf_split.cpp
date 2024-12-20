#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include "poppler/cpp/poppler-document.h"
#include "poppler/cpp/poppler-page.h"
#include "poppler/cpp/poppler-image.h"

namespace fs = std::filesystem;

// Struct to store results for writing
struct PageResult {
    std::string pdf_name;
    int page_id;
    std::string text;
    std::vector<std::pair<int, poppler::image>> images;
};

// Worker function to process a single page
PageResult process_page(poppler::page* page, const std::string& pdf_name, int page_id) {
    PageResult result;
    result.pdf_name = pdf_name;
    result.page_id = page_id;

    if (!page) return result;

    // Extract text layer
    std::vector<char> text_t = page->text().to_utf8();
    std::string text(text_t.begin(), text_t.end());
    result.text = text;

    // Extract images
    int image_count = 0;
    
    /*for (const auto& img : page->images()) {
        poppler::image poppler_img = img;
        if (poppler_img.is_valid()) {
            result.images.emplace_back(image_count++, poppler_img);
        }
    }*/

    return result;
}

// Thread-safe queue for page results
class PageResultQueue {
public:
    void push(PageResult&& result) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(std::move(result));
        cond_var_.notify_one();
    }

    bool pop(PageResult& result) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.wait(lock, [this]() { return !queue_.empty() || finished_; });
        if (queue_.empty()) return false;
        result = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void set_finished() {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_ = true;
        cond_var_.notify_all();
    }

private:
    std::queue<PageResult> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;
    bool finished_ = false;
};

void writer_thread(PageResultQueue& queue, const fs::path& output_dir) {
    PageResult result;
    while (queue.pop(result)) {
        // Create output directory for the page
        fs::path page_dir = output_dir / (result.pdf_name + "/" + std::to_string(result.page_id));
        fs::create_directories(page_dir);

        // Write text layer
        fs::path text_file = page_dir / "text_layer.txt";
        std::ofstream ofs(text_file);
        ofs << result.text;

        // Write images
        /*
        for (const auto& [image_id, image] : result.images) {
            fs::path image_file = page_dir / ("image_" + std::to_string(image_id) + ".png");
            image.save(image_file.string().c_str(), "png");
        }*/
    }
}

void process_pdf(const fs::path& pdf_path, const fs::path& output_dir, int thread_count) {
    std::string pdf_name = pdf_path.stem().string();

    // Open PDF document
    poppler::document* doc = poppler::document::load_from_file(pdf_path.string());
    if (!doc) {
        std::cerr << "Failed to open PDF: " << pdf_path << std::endl;
        return;
    }

    int num_pages = doc->pages();
    std::atomic<int> current_page(0);
    PageResultQueue result_queue;

    // Worker threads for processing pages
    auto worker = [&]() {
        while (true) {
            int page_id = current_page.fetch_add(1);
            if (page_id >= num_pages) break;

            poppler::page* page = doc->create_page(page_id);
            if (page) {
                PageResult result = process_page(page, pdf_name, page_id);
                result_queue.push(std::move(result));
                delete page;
            }
        }
        };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker);
    }

    // Writer thread for writing results
    std::thread writer(writer_thread, std::ref(result_queue), std::ref(output_dir));

    // Join worker threads
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    // Notify writer thread to finish
    result_queue.set_finished();
    if (writer.joinable()) writer.join();

    delete doc;
}

void process_directory(const fs::path& input_dir, const fs::path& output_dir, int thread_count) {
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".pdf") {
            process_pdf(entry.path(), output_dir, thread_count);
        }
    }
}

int main(int argc, char* argv[]) {
    
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <input_dir> <output_dir> <thread_count>" << std::endl;
        return 1;
    }
    setlocale(LC_ALL, "Russian");
    fs::path input_dir = argv[1];
    fs::path output_dir = argv[2];
    int thread_count = std::stoi(argv[3]);

    if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
        std::cerr << "Input directory does not exist or is not a directory." << std::endl;
        return 1;
    }
    

    fs::create_directories(output_dir);

    process_directory(input_dir, output_dir, thread_count);

    return 0;
}
