#include <bencode/bencode_parser.hpp>
#include <config.hpp>
#include <fstream>
#include <furrent.hpp>
#include <iostream>
#include <log/logger.hpp>
#include <platform/io.hpp>
#include <policy/policy.hpp>
#include <random>

namespace fur {

/// Constructs a new empty piece task
PieceTask::PieceTask() : _data{std::nullopt}, tid{0} {}

/// Constructs a new piece task
PieceTask::PieceTask(TorrentID tid, Piece piece, const TorrentFile& descriptor)
    : _data{std::nullopt},
      tid{tid},
      piece{std::move(piece)},
      descriptor{descriptor} {}

/// Process piece, downloads it from a peer and saves it to file
PieceTaskStats PieceTask::process(const peer::Peer& peer) {
  PieceTaskStats stats{};
  stats.completed = download(peer) && save();
  return stats;
}

/// Download the PieceTask from the provided peer and inserts it into the _data
/// member variable.
bool PieceTask::download(const peer::Peer& peer) {
  auto logger = spdlog::get("custom");
  auto clock_beg = std::chrono::high_resolution_clock::now();

  download::downloader::Downloader d(descriptor, peer);
  auto download = d.try_download(piece);
  if (!download.valid()) {
    logger->trace("Error while downloading piece [{:4}] of T{} from {}",
                  piece.index, tid, peer.address());
    return false;
  }

  auto clock_end = std::chrono::high_resolution_clock::now();
  auto clock_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock_end - clock_beg);

  logger->info("Downloaded piece [{:4}] of T{} from {} ({} ms)", piece.index,
               tid, peer.address(), clock_elapsed.count());

  std::optional<download::Downloaded> data(*download);
  _data.swap(data);

  return true;
}

/// Save to file
bool PieceTask::save() const {
  auto logger = spdlog::get("custom");

  // Write every subpiece
  for (const auto& subpiece : piece.subpieces) {
    const std::string filepath =
        descriptor.folder_name + '/' + subpiece.filepath;
    const auto maybe_write = fur::platform::io::write_bytes(
        filepath, _data.value().content, subpiece.file_offset);

    if (!maybe_write.valid()) {
      logger->error("Error while saving piece [{:4}] of T{} to {}", piece.index,
                    tid, piece.subpieces[0].filepath);
      return false;
    }
  }

  logger->info("Saved piece [{:4}] of T{} to {}", piece.index, tid,
               piece.subpieces[0].filepath);
  return true;
}

// ======================================================================================

Furrent::Furrent() : _descriptor_next_uid{0u}, _download_folder{"."} {
  // Default global logger
  auto logger = spdlog::get("custom");

  using std::placeholders::_1;
  using std::placeholders::_2;
  using std::placeholders::_3;

  /// This is the core of all workers
  const int64_t concurrency = std::thread::hardware_concurrency();
  const int64_t threads_cnt = (concurrency > 1) ? concurrency - 1 : 1;

  logger->info(
      "Launching workers threads (concurrency capability: {}, workers: {})",
      concurrency, threads_cnt);

  _workers.launch(std::bind(&Furrent::thread_main, this, _1, _2, _3),
                  threads_cnt);
}

Furrent::~Furrent() {
  _tasks.begin_skip_waiting();
  _workers.terminate();
}

auto Furrent::set_download_folder(const std::string& folder) -> Result<Empty> {
  // Default global logger
  auto logger = spdlog::get("custom");

  auto existence = fur::platform::io::exists(folder);
  if (!existence.valid()) {
    logger->error("Unable to check existence of download folder at {}", folder);
    return Result<Empty>::ERROR(Error::GenericError);
  }

  // If the folder doesn't exists create it
  if (!*existence) {
    logger->info("Download folder doesn't exists, creating it at {}", folder);
    auto creation = fur::platform::io::create_directories(folder);
    if (!creation.valid()) {
      logger->error("Unable to create download folder at {}", folder);
      return Result<Empty>::ERROR(Error::GenericError);
    }
  }

  logger->info("Valid download folder at {}", folder);
  _download_folder = folder;
  return Result<Empty>::OK({});
}

/// Print the peers distribution of a torrent
static void thread_print_torrent_stats(
    std::mt19937& gen, PieceTask& task, const std::vector<peer::Peer>& peers,
    std::discrete_distribution<int64_t>& distr) {
  std::vector<int64_t> rolls(peers.size());
  for (int i = 0; i < 10000; i++) rolls[distr(gen)] += 1;

  // Number of peers is guaranteed to fit in an int64_t, see torrent.cpp

  std::stringstream ss;
  for (int64_t i = 0; i < static_cast<int64_t>(peers.size()); i++) {
    ss.width(30);
    ss << std::right << peers[i].address();
    ss.width(0);
    ss << " : "
       << std::string(rolls[i] * static_cast<int64_t>(peers.size()) / 1000,
                      '*');
    ss << std::endl;
  }

  auto logger = spdlog::get("custom");
  logger->info("Peers distribution for T[{}]:\n{}", task.tid, ss.str());
}

void Furrent::thread_main(mt::Runner runner, WorkerState& state,
                          int64_t index) {
  // Default global logger
  auto logger = spdlog::get("custom");

  // Random generator for each thread
  thread_local std::random_device rng;
  thread_local std::mt19937 gen(rng());

  policy::LIFOPolicy<PieceTask> piece_policy;
  while (runner.alive()) {
    // Try to extract
    auto extraction = _tasks.try_extract(piece_policy);
    if (extraction.valid()) {
      PieceTask task = *extraction;
      std::discrete_distribution<int64_t> peers_distribution;
      std::vector<peer::Peer> peers;

      // TODO: update peers if necessary, for now peers are constant!
      {
        // Lock against writes to the _torrents map
        std::shared_lock<std::shared_mutex> lock(_mtx);
        const Torrent& torrent = _torrents[task.tid];

        // If the torrent is paused then skip processing and add
        // task to queue again
        if (torrent.state.load(std::memory_order_relaxed) ==
            TorrentState::Paused) {
          _tasks.emplace(task.tid, task.piece, torrent.descriptor());
          continue;
        }

        // Generate peers score distribution
        peers_distribution = torrent.distribution();
        peers = torrent.peers();
      }

      while (true) {
        int64_t peer_index = peers_distribution(gen);
        PieceTaskStats stats = task.process(peers[peer_index]);
        if (!stats.completed) {
          continue;
        }

        state.piece_processed += 1;

        // Lock against writes to the _torrents map
        std::shared_lock<std::shared_mutex> lock(_mtx);
        Torrent& torrent = _torrents[task.tid];

        // Update score of used peer
        torrent.atomic_add_peer_score(peer_index);
        int64_t processed =
            torrent.pieces_processed.fetch_add(1, std::memory_order_relaxed) +
            1;

        // Show peers score distribution every 100 pieces processed
        if (processed % 100 == 0)
          thread_print_torrent_stats(gen, task, peers, peers_distribution);

        // Change state to completed if there are no more pieces to process
        if (processed == torrent.descriptor().pieces_count) {
          torrent.state.exchange(TorrentState::Completed,
                                 std::memory_order_relaxed);
          logger->info("Completed T[{}]", torrent.tid());
        }
        break;
      }
    }

    // Extraction failure or no more elements
    else {
      switch (extraction.error()) {
        // No more work to do
        case policy::Queue<PieceTask>::Error::Empty: {
          logger->info("thread {:02d} is waiting for work, queue is empty",
                       index);
          _tasks.wait_work();
          break;
        }

        // Policy failed to return an element
        case policy::Queue<PieceTask>::Error::PolicyFailure: {
          logger->info(
              "thread {:02d} is waiting for work, policy extraction returned "
              "nothing",
              index);
          _tasks.wait_work();
          break;
        }
      }
    }
  }
}

bool Furrent::prepare_torrent_files(TorrentFile& descriptor) {
  using namespace fur::platform;  // For IO operations

  std::string torrent_base_path = _download_folder + '/' + descriptor.name;
  auto existence = io::exists(torrent_base_path);

  const int64_t MAX_COPY_ATTEMPTS = 10;
  int64_t attempts = 0;

  // If directory already exists then keep adding "COPY"
  while (attempts < MAX_COPY_ATTEMPTS && existence.valid() && *existence) {
    torrent_base_path += " COPY";
    existence = io::exists(torrent_base_path);
    attempts += 1;
  }

  // If we tried to many times to generate new folders copy
  if (attempts > MAX_COPY_ATTEMPTS) return false;

  // Create output directory
  auto torrent_dirpath = io::create_directories(torrent_base_path);
  if (!torrent_dirpath.valid()) return false;

  // Create output files
  bool must_cleanup = false;
  descriptor.folder_name = *torrent_dirpath;
  for (const auto& file : descriptor.files) {
    // Create nested folders
    auto file_dirpath = io::create_directories(
        descriptor.folder_name + '/' + file.filename(), true);
    if (!file_dirpath.valid()) {
      must_cleanup = true;
      break;
    }

    // Create single output file
    std::string filepath = *file_dirpath + '/' + file.filepath.back();
    auto creation = io::touch(filepath, file.length);
    if (!creation.valid()) {
      must_cleanup = true;
      break;
    }
  }

  // Remove created content if we failed to create all files
  if (must_cleanup) {
    io::remove(descriptor.folder_name);
    return false;
  }

  return true;
}

/// Begin download of a torrent
auto Furrent::add_torrent(const std::string& filename) -> Result<TorrentID> {
  auto logger = spdlog::get("custom");

  TorrentID tid = _descriptor_next_uid;
  _descriptor_next_uid += 1;

  // Load .torrent content
  auto reading = fur::platform::io::load_file_text(filename);
  if (!reading.valid()) {
    logger->critical("Error loading torrent T{} from file [{}]", tid, filename);
    return Result<TorrentID>::ERROR(Error::LoadingTorrentFailed);
  }

  // Parse content
  auto parser = fur::bencode::BencodeParser();
  auto btree = parser.decode(*reading);
  if (!btree.valid()) {
    logger->critical("Error parsing torrent T{} described at [{}]", tid,
                     filename);
    return Result<TorrentID>::ERROR(Error::LoadingTorrentFailed);
  }

  // Create new torrent object and mapped files
  TorrentFile descriptor(*(*btree));
  if (!prepare_torrent_files(descriptor)) {
    logger->critical("Error preparing torrent T{} described at [{}]", tid,
                     filename);
    return Result<TorrentID>::ERROR(Error::LoadingTorrentFailed);
  }

  logger->info("Announcing T{} to tracker at {}", tid, descriptor.announce_url);

  // Lock against concurrent read/write of the _torrents map
  std::unique_lock<std::shared_mutex> lock(_mtx);
  // This creates the Torrent instance and makes the announcement to the tracker
  _torrents.try_emplace(tid, tid, descriptor);
  Torrent& torrent = _torrents[tid];

  // Popolate peers
  std::stringstream ss;
  ss << "Peers:\n";
  for (auto& peer : torrent.peers()) ss << "  " << peer.address() << "\n";
  logger->info("{}", ss.str());

  // Create a task for each piece
  logger->info("Generating {} pieces for T{} ", descriptor.pieces_count, tid);
  std::vector<Piece> pieces = torrent.pieces();
  for (Piece& piece : pieces) _tasks.emplace(tid, piece, torrent.descriptor());

  torrent.state.exchange(TorrentState::Downloading);
  return Result<TorrentID>::OK(std::move(tid));
}

/// Removes a torrent descriptor and all of his tasks
void Furrent::remove_torrent(TorrentID tid) {
  // Remove all tasks refering to the removed torrent
  _tasks.mutate([&](PieceTask& task) -> bool { return task.tid == tid; });

  // Lock against writes to _torrents map
  std::shared_lock<std::shared_mutex> lock(_mtx);
  Torrent& torrent = _torrents[tid];

  TorrentState state = torrent.state.load(std::memory_order_relaxed);
  if (state != TorrentState::Completed && state != TorrentState::Error)
    _torrents[tid].state.exchange(TorrentState::Stopped,
                                  std::memory_order_relaxed);
}

// Extract torrents stats
TorrentGuiData Furrent::get_gui_data(TorrentID target_tid) const {
  // Lock against writes to _torrents map
  std::shared_lock<std::shared_mutex> lock(_mtx);
  for (const auto& [tid, torrent] : _torrents) {
    if (target_tid == tid) {
      const TorrentFile& d = torrent.descriptor();
      return TorrentGuiData{tid, torrent.state.load(), d.name,
                            torrent.pieces_processed.load(), d.pieces_count};
    }
  }

  throw std::invalid_argument("asked for a torrent id that doesn't exist");
}

}  // namespace fur
