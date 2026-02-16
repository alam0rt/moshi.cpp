//
// moshi-mumble.cpp – Moshi speech-to-speech via Mumble VoIP
//
// Connects to a Mumble server as a bot, receives voice from other users,
// feeds it through the Moshi LM pipeline (encode → LM → decode), and
// sends the response audio back to the channel.
//
// Modelled on moshi-sts.cpp, replacing SDL audio I/O with MumbleClient.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <signal.h>
#include <chrono>
#include <thread>

#include <moshi/moshi.h>
#include "mumble_client.h"
#include "util.h"

static void print_usage(const char *program) {
    fprintf(stderr, R"(usage: %s [option(s)]

connects to a mumble server and participates as a speech-to-speech bot.

options:
  -h,       --help             shows this help message

  mumble options:
            --host HOST        mumble server hostname (default: localhost)
            --port PORT        mumble server port (default: 64738)
  -u NAME,  --username NAME    bot username (default: moshi-bot)
            --password PASS    server password if required
            --channel NAME     channel to join after connecting

  hardware options:
  -d NAME,  --device NAME      use named ggml hardware device
  -l,       --list-devices     list hardware devices and exit
            --threads N        number of CPU threads

  model options:
  -r PATH,  --model-root PATH  path to where all kyutai models are stored and
                               replaces MODEL_CACHE environment variable. the
                               models at root are in subdirectories of
                               'organization/model'
  -m PATH,  --model PATH       path to where model is, can be relative to the
                               MODEL_CACHE environment variable, or program
                               directory, or working directory. by default is
                               'Codes4Fun/moshika-q4_k-GGUF'
  -q QUANT, --quantize QUANT   convert weights to: q8_0, q4_0, q4_k
  -g,       --gguf-caching     loads gguf if exists, saves gguf if it does not.
                               model is saved alongside the original
                               safetensors file.

  -c N,     --context N        default: 3000
  -s N,     --seed N           seed value
  -t N,     --temperature N    consistency vs creativity, default 0.8

personaplex options:
  -v NAME,  --voice NAME       either a filepath to a safetensor or one of:
                                    NATF0 NATF1 NATF2 NATF3
                                    NATM0 NATM1 NATM2 NATM3
                                    VARF0 VARF1 VARF2 VARF3 VARF4
                                    VARM0 VARM1 VARM2 VARM3 VARM4

)", program);
    exit(1);
}

static bool shutdown_flag = false;
static int64_t lm_delta_time = 0;
static int64_t lm_frames = 0;

static void log_metrics() {
    printf("\n\nrun frames: %d\n", (int)lm_frames);
    printf("run time: %.3f s\n", lm_delta_time / 1000000.f);
    printf("\nframe rate:  %f frames/s\n",
           lm_frames * 1000000.f / lm_delta_time);
}

static void signal_handler(int) {
    shutdown_flag = true;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);

    const char *device = NULL;
    int n_threads = 0;

    const char *model_cache = getenv("MODEL_CACHE");
    std::string model_root = model_cache ? model_cache : "";
    std::string model_path = "Codes4Fun/moshika-q4_k-GGUF/";
    bool model_path_set = false;
    bool personaplex = false;
    const char *quant = NULL;
    bool gguf_caching = false;

    int context = -1;
    int seed = (int)time(NULL);
    float depth_temperature = 0.8f;
    float text_temperature = 0.7f;

    std::string personaplex_voice_filepath = "";

    // Mumble options
    std::string mumble_host = "localhost";
    int         mumble_port = 64738;
    std::string mumble_user = "moshi-bot";
    std::string mumble_pass = "";
    std::string mumble_chan = "";

    //////////////////////
    // MARK: Parse Args
    //////////////////////

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
        }
        if (arg == "-l" || arg == "--list-devices") {
            list_devices();
        }
        if (arg == "-d" || arg == "--device") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires name of device\n", argv[i]); exit(1); }
            device = argv[++i]; continue;
        }
        if (arg == "--threads") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires value\n", argv[i]); exit(1); }
            n_threads = std::stoi(argv[++i]); continue;
        }
        if (arg == "-r" || arg == "--model-root") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires path\n", argv[i]); exit(1); }
            model_root = argv[++i]; continue;
        }
        if (arg == "-m" || arg == "--model") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires filepath\n", argv[i]); exit(1); }
            model_path = argv[++i]; model_path_set = true;
            personaplex = model_path.find("personaplex") != std::string::npos;
            continue;
        }
        if (arg == "-q" || arg == "--quantize") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires type\n", argv[i]); exit(1); }
            quant = argv[++i]; continue;
        }
        if (arg == "-g" || arg == "--gguf-caching") {
            gguf_caching = true; continue;
        }
        if (arg == "-c" || arg == "--context") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires value\n", argv[i]); exit(1); }
            context = std::stoi(argv[++i]); continue;
        }
        if (arg == "-s" || arg == "--seed") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires value\n", argv[i]); exit(1); }
            seed = std::stoi(argv[++i]); continue;
        }
        if (arg == "-t" || arg == "--temperature") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires value\n", argv[i]); exit(1); }
            text_temperature = (float)std::stod(argv[++i]);
            depth_temperature = text_temperature; continue;
        }
        if (arg == "-v" || arg == "--voice") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires voice name\n", argv[i]); exit(1); }
            personaplex_voice_filepath = argv[++i]; continue;
        }
        // Mumble-specific
        if (arg == "--host") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires hostname\n", argv[i]); exit(1); }
            mumble_host = argv[++i]; continue;
        }
        if (arg == "--port") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires port\n", argv[i]); exit(1); }
            mumble_port = std::stoi(argv[++i]); continue;
        }
        if (arg == "-u" || arg == "--username") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires username\n", argv[i]); exit(1); }
            mumble_user = argv[++i]; continue;
        }
        if (arg == "--password") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires password\n", argv[i]); exit(1); }
            mumble_pass = argv[++i]; continue;
        }
        if (arg == "--channel") {
            if (i + 1 >= argc) { fprintf(stderr, "error: \"%s\" requires channel name\n", argv[i]); exit(1); }
            mumble_chan = argv[++i]; continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "error: unrecognized option \"%s\"\n", argv[i]); exit(1);
        }
        fprintf(stderr, "error: unexpected extra argument \"%s\"\n", argv[i]); exit(1);
    }

    /////////////////////////
    // MARK: Initialize
    /////////////////////////

    std::string program_path = get_program_path(argv[0]);
    ensure_path(program_path);
    ensure_path(model_root);
    ensure_path(model_path);

    // --- find model path (same logic as moshi-sts.cpp) ---
    bool found_file, found_dir;
    if (is_abs_or_rel(model_path)) {
        check_arg_path(model_path, found_file, found_dir);
        if (!found_dir) {
            if (found_file) {
                fprintf(stderr, "error: expected directory but found file: %s\n", model_path.c_str());
            } else {
                fprintf(stderr, "error: could not find directory: %s\n", model_path.c_str());
            }
            exit(1);
        }
    } else if (!model_path_set) {
        std::vector<std::string> paths;
        paths.push_back(model_root + "Codes4Fun/moshika-q4_k-GGUF/");
        paths.push_back(program_path + "Codes4Fun/moshika-q4_k-GGUF/");
        paths.push_back("Codes4Fun/moshika-q4_k-GGUF/");
        paths.push_back(model_root + "kyutai/moshika-pytorch-bf16/");
        paths.push_back(program_path + "kyutai/moshika-pytorch-bf16/");
        paths.push_back("kyutai/moshika-pytorch-bf16/");
        found_dir = false;
        for (auto &path : paths) {
            check_arg_path(path, found_file, found_dir);
            if (found_dir) { model_path = path; break; }
        }
        if (!found_dir) {
            fprintf(stderr, "error: could not find a default model directory\n");
            exit(1);
        }
    } else {
        std::string full_path = model_root + model_path;
        check_arg_path(full_path, found_file, found_dir);
        if (found_dir) {
            model_path = full_path;
        } else {
            full_path = program_path + model_path;
            check_arg_path(full_path, found_file, found_dir);
            if (found_dir) {
                model_path = full_path;
            } else {
                check_arg_path(model_path, found_file, found_dir);
                if (!found_dir) {
                    fprintf(stderr, "error: could not find directory: %s\n", model_path.c_str());
                    exit(1);
                }
            }
        }
    }
    printf("found model path: %s\n", model_path.c_str());

    // --- config ---
    moshi_config_t config;
    std::string config_filepath;
    if (personaplex) {
        config_filepath = model_path + "personaplex-config.json";
        if (!file_exists(config_filepath.c_str())) {
            config_filepath = program_path + "personaplex-config.json";
            if (!file_exists(config_filepath.c_str())) {
                fprintf(stderr, "error: failed to find a config.json\n"); exit(1);
            }
        }
    } else {
        config_filepath = model_path + "config.json";
        if (!file_exists(config_filepath.c_str())) {
            config_filepath = program_path + "moshi-config.json";
            if (!file_exists(config_filepath.c_str())) {
                fprintf(stderr, "error: failed to find a config.json\n"); exit(1);
            }
        }
    }

    if (moshi_get_config(&config, config_filepath.c_str()) != 0) {
        fprintf(stderr, "error: reading config\n"); exit(1);
    }

    if (context > 0) config.context = context;

    std::string model_filepath = model_path + config.moshi_name;
    std::string mimi_filepath = model_path + config.mimi_name;
    std::string tokenizer_filepath = model_path + config.tokenizer_name;

    if (!file_exists(model_filepath.c_str())) {
        fprintf(stderr, "error: missing moshi file \"%s\"\n", model_filepath.c_str());
        exit(1);
    }

    // --- search for mimi if not at expected path ---
    if (!file_exists(mimi_filepath.c_str())) {
        bool found = false;
        std::vector<std::string> paths = {
            "kyutai/tts-1.6b-en_fr/tokenizer-e351c8d8-checkpoint125.safetensors",
            "kyutai/tts-0.75b-en-public/tokenizer-e351c8d8-checkpoint125.safetensors",
            "kyutai/stt-1b-en_fr-candle/mimi-pytorch-e351c8d8@125.safetensors",
            "kyutai/stt-2.6b-en/mimi-pytorch-e351c8d8@125.safetensors",
            "kyutai/stt-1b-en_fr/mimi-pytorch-e351c8d8@125.safetensors",
        };
        if (model_root.size()) {
            for (auto &p : std::vector<std::string>(paths))
                paths.push_back(model_root + p);
        }
        if (program_path.size()) {
            for (auto &p : std::vector<std::string>(paths))
                paths.push_back(program_path + p);
        }
        for (auto &path : paths) {
            if (file_exists(path.c_str())) { mimi_filepath = path; found = true; break; }
        }
        if (!found) {
            fprintf(stderr, "error: missing mimi file \"%s\"\n", mimi_filepath.c_str());
            exit(1);
        }
    }

    // --- search for tokenizer if not at expected path ---
    if (!file_exists(tokenizer_filepath.c_str())) {
        bool found = false;
        if (config.tokenizer_name == "tokenizer_spm_32k_3.model") {
            std::vector<std::string> paths = {
                "kyutai/moshika-pytorch-bf16/tokenizer_spm_32k_3.model",
                "kyutai/moshiko-pytorch-bf16/tokenizer_spm_32k_3.model"
            };
            if (model_root.size()) {
                paths.push_back(model_root + "kyutai/moshika-pytorch-bf16/tokenizer_spm_32k_3.model");
                paths.push_back(model_root + "kyutai/moshiko-pytorch-bf16/tokenizer_spm_32k_3.model");
            }
            if (program_path.size()) {
                paths.push_back(program_path + "kyutai/moshika-pytorch-bf16/tokenizer_spm_32k_3.model");
                paths.push_back(program_path + "kyutai/moshiko-pytorch-bf16/tokenizer_spm_32k_3.model");
            }
            for (auto &path : paths) {
                if (file_exists(path.c_str())) { tokenizer_filepath = path; found = true; break; }
            }
        }
        if (!found) {
            fprintf(stderr, "error: missing tokenizer file \"%s\"\n", tokenizer_filepath.c_str());
            exit(1);
        }
    }

    // --- personaplex voice search ---
    if (personaplex && personaplex_voice_filepath.size()
        && !file_exists(personaplex_voice_filepath.c_str())) {
        std::vector<std::string> paths;
        if (personaplex_voice_filepath.size() == 5) {
            std::string expanded = model_path + "voices/" + personaplex_voice_filepath;
            paths.push_back(expanded + ".gguf");
            paths.push_back(expanded + ".safetensors");
        }
        paths.push_back(model_path + personaplex_voice_filepath);
        if (model_root.size()) paths.push_back(model_root + personaplex_voice_filepath);
        if (program_path.size()) paths.push_back(program_path + personaplex_voice_filepath);

        bool found = false;
        for (auto &path : paths) {
            if (file_exists(path.c_str())) { personaplex_voice_filepath = path; found = true; break; }
        }
        if (!found) {
            fprintf(stderr, "error: failed to find voice file \"%s\"\n",
                    personaplex_voice_filepath.c_str());
            exit(1);
        }
    }

    ////////////////////
    // MARK: Loading
    ////////////////////

    srand(seed);
    printf("seed: %d\n", seed);

    unref_ptr<moshi_context_t> moshi = moshi_alloc(device);
    if (n_threads > 0 && n_threads < 512) {
        moshi_set_n_threads(moshi, n_threads);
        printf("set threads to %d\n", n_threads);
    }

    printf("loading...\n");
    auto load_start = ggml_time_ms();

    if (quant) {
        uint32_t uquant = *(uint32_t *)quant;
        switch (uquant) {
        case 0x305f3471: break; // "q4_0"
        case 0x6b5f3471: break; // "q4_k"
        case 0x305f3871: break; // "q8_0"
        default:
            fprintf(stderr, "error: invalid quant %s\n", quant); exit(-1);
        }
    }

    std::string model_gguf = "";
    if (gguf_caching) {
        if (quant) {
            model_gguf = model_filepath + "." + quant + ".gguf";
            if (file_exists(model_gguf.c_str())) {
                model_filepath = model_gguf; model_gguf = ""; quant = NULL;
            }
        } else {
            model_gguf = model_filepath + ".gguf";
            if (file_exists(model_gguf.c_str())) {
                model_filepath = model_gguf; model_gguf = "";
            }
        }
    }

    unref_ptr<moshi_lm_t> lm = moshi_lm_from_files(moshi, &config,
                                                     model_filepath.c_str());
    if (quant) {
        if (!moshi_lm_quantize(lm, quant)) {
            fprintf(stderr, "error: unknown quant %s\n", quant); exit(-1);
        }
    }

    unref_ptr<moshi_lm_gen_t> gen = moshi_lm_generator(lm);

    unref_ptr<tokenizer_t> tok = tokenizer_alloc(
        tokenizer_filepath.c_str(), config.cross_attention);

    int num_codebooks = (int)(config.n_q - config.dep_q);
    if (config.dep_q >= config.n_q) num_codebooks = (int)config.dep_q;
    if (personaplex) num_codebooks = 8;

    unref_ptr<mimi_codec_t> codec = mimi_alloc(moshi,
        mimi_filepath.c_str(), num_codebooks);
    float frame_rate = mimi_frame_rate(codec);
    int frame_size = mimi_frame_size(codec);

    assert(frame_size == MumbleClient::MOSHI_FRAME_SIZE);

    moshi_lm_load(lm);
    if (model_gguf.size()) {
        moshi_lm_save_gguf(lm, model_gguf.c_str());
    }

    unref_ptr<mimi_encode_context_t> encoder = mimi_encode_alloc_context(codec);
    unref_ptr<mimi_decode_context_t> decoder = mimi_decode_alloc_context(codec);

    auto load_end = ggml_time_ms();
    printf("done loading. %f s\n", (load_end - load_start) / 1000.f);

    if (personaplex && personaplex_voice_filepath.size()) {
        moshi_lm_personaplex_load_voice(moshi, gen, personaplex_voice_filepath.c_str());
    }

    /////////////////////////////
    // MARK: Mumble Connect
    /////////////////////////////

    MumbleClient mumble;
    printf("connecting to mumble://%s:%d as \"%s\"...\n",
           mumble_host.c_str(), mumble_port, mumble_user.c_str());

    if (!mumble.connect(mumble_host, mumble_port, mumble_user,
                        mumble_pass, mumble_chan)) {
        fprintf(stderr, "error: failed to connect to Mumble server\n");
        exit(1);
    }

    /////////////////////////
    // MARK: Loop
    /////////////////////////

    moshi_lm_start(moshi, gen, depth_temperature, text_temperature);

    std::vector<int16_t> tokens(num_codebooks);
    int text_token;
    std::vector<float> input_frame(frame_size);
    std::vector<float> output_frame(frame_size);

    printf("mumble: listening... (Ctrl+C to stop)\n");

    uint64_t lm_start = ggml_time_us();
    while (!shutdown_flag && mumble.is_connected()) {

        // --- Audio input from Mumble ---
        bool got_audio = mumble.receive_audio(input_frame.data());

        if (!got_audio) {
            // No audio from other users — feed silence and throttle to real-time
            memset(input_frame.data(), 0, frame_size * sizeof(float));
            // Sleep ~80 ms (one Moshi frame) to avoid busy-spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (int)(1000.0f / frame_rate)));
        }

        lm_start = ggml_time_us();

        // --- Encode PCM → audio tokens ---
        mimi_encode_send(encoder, input_frame.data());
        mimi_encode_receive(encoder, tokens.data());

        // --- LM inference ---
        moshi_lm_send2(gen, tokens);

        if (moshi_lm_receive(gen, text_token, tokens)) {
            // --- Decode audio tokens → PCM ---
            mimi_decode_send(decoder, tokens.data());
            mimi_decode_receive(decoder, output_frame.data());

            // --- Send response audio to Mumble ---
            mumble.send_audio(output_frame.data());

            lm_delta_time += ggml_time_us() - lm_start;
            lm_frames++;

            // --- Text output ---
            if (text_token != 0 && text_token != 3) {
                auto piece = tokenizer_id_to_piece(tok, text_token);
                std::string _text;
                for (size_t ci = 0; ci < piece.size(); ci++) {
                    if (piece.c_str()[ci] == -30) {
                        _text += ' ';
                        ci += 2;
                        continue;
                    }
                    _text += piece[ci];
                }
                fprintf(stdout, "%s", _text.c_str());
                fflush(stdout);
            }
        }
    }

    log_metrics();

    printf("\nmumble: disconnecting...\n");
    mumble.disconnect();

    return 0;
}
