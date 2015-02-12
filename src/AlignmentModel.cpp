#include <iostream>
#include <cstdio>

#include <boost/config.hpp> // for BOOST_LIKELY/BOOST_UNLIKELY

#include "AlignmentModel.hpp"
#include "Transcript.hpp"
#include "SailfishMath.hpp"
#include "SailfishStringUtils.hpp"
#include "UnpairedRead.hpp"
#include "ReadPair.hpp"

AlignmentModel::AlignmentModel(double alpha, uint32_t maxExpectedReadLen, uint32_t readBins ) :
    maxExpectedLen_(maxExpectedReadLen),
    transitionProbsLeft_(readBins, AtomicMatrix<double>(numAlignmentStates(), numAlignmentStates(), alpha)),
    transitionProbsRight_(readBins, AtomicMatrix<double>(numAlignmentStates(), numAlignmentStates(), alpha)),
    isEnabled_(true),
    maxLen_(0),
    readBins_(readBins),
    burnedIn_(false) {}

bool AlignmentModel::burnedIn() { return burnedIn_; }
void AlignmentModel::burnedIn(bool burnedIn) { burnedIn_ = burnedIn; }


bool AlignmentModel::hasIndel(ReadPair& hit) {
    if (!hit.isPaired()) {
        return hasIndel(hit.read1);
    }
    return hasIndel(hit.read1) or hasIndel(hit.read2);
}


bool AlignmentModel::hasIndel(UnpairedRead& hit) {
    return hasIndel(hit.read);
}


bool AlignmentModel::hasIndel(bam_seq_t* read) {
    uint32_t* cigar = bam_cigar(read);
    uint32_t cigarLen = bam_cigar_len(read);

    for (uint32_t cigarIdx = 0; cigarIdx < cigarLen; ++cigarIdx) {
        uint32_t opLen = cigar[cigarIdx] >> BAM_CIGAR_SHIFT;
        enum cigar_op op = static_cast<enum cigar_op>(cigar[cigarIdx] & BAM_CIGAR_MASK);

        switch (op) {
            case BAM_CINS:
                return true;
            case BAM_CDEL:
                return true;
            default:
                break;
        }
    }
    return false;
}

double AlignmentModel::logLikelihood(bam_seq_t* read, Transcript& ref,
                                 std::vector<AtomicMatrix<double>>& transitionProbs){
    using namespace sailfish::stringtools;
    bool useQual{false};
    size_t readIdx{0};
    auto transcriptIdx = bam_pos(read);
    size_t transcriptLen = ref.RefLength;
    // if the read starts before the beginning of the transcript,
    // only consider the part overlapping the transcript
    if (transcriptIdx < 0) {
        readIdx = -transcriptIdx;
        transcriptIdx = 0;
    }

    // unsigned version of transcriptIdx
    size_t uTranscriptIdx = static_cast<size_t>(transcriptIdx);

    if (uTranscriptIdx >= transcriptLen) {
        std::lock_guard<std::mutex> l(outputMutex_);
        std::cerr << "transcript index = " << uTranscriptIdx << ", transcript length = " << transcriptLen << "\n";
        return sailfish::math::LOG_0;
    }

    uint32_t* cigar = bam_cigar(read);
    uint32_t cigarLen = bam_cigar_len(read);
    uint8_t* qseq = reinterpret_cast<uint8_t*>(bam_seq(read));
    uint8_t* qualStr = reinterpret_cast<uint8_t*>(bam_qual(read));

    if (cigarLen == 0 or !cigar) { return sailfish::math::LOG_EPSILON; }

    sailfish::stringtools::strand readStrand = sailfish::stringtools::strand::forward;
    double logLike = sailfish::math::LOG_1;

    /*
    std::stringstream readStr;
    std::stringstream matchStr;
    std::stringstream refStr;
    std::stringstream stateStr;
    */

    uint32_t readPosBin{0};
    uint32_t cigarIdx{0};
    uint32_t prevStateIdx{startStateIdx};
    uint32_t curStateIdx{0};
    double invLen = static_cast<double>(readBins_) / bam_seq_len(read);

    for (uint32_t cigarIdx = 0; cigarIdx < cigarLen; ++cigarIdx) {
        uint32_t opLen = cigar[cigarIdx] >> BAM_CIGAR_SHIFT;
        enum cigar_op op = static_cast<enum cigar_op>(cigar[cigarIdx] & BAM_CIGAR_MASK);
        size_t curReadBase = samToTwoBit[bam_seqi(qseq, readIdx)];
        size_t curRefBase = samToTwoBit[ref.baseAt(uTranscriptIdx, readStrand)];

        //stateStr << prevStateIdx << ", ";
        for (size_t i = 0; i < opLen; ++i) {
           switch (op) {
                    case BAM_UNKNOWN:
                        std::cerr << "ENCOUNTERED UNKNOWN SYMBOL IN CIGAR STRING!\n";
                        break;
                    case BAM_CMATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ((curReadBase == curRefBase) ? ' ' : 'X');
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CBASE_MATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CBASE_MISMATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << 'X';
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CINS:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << '-';
                        */
                        curRefBase = ALN_DASH;
                        break;
                    case BAM_CDEL:
                        /*
                        readStr << '-';
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        curReadBase = ALN_DASH;
                        break;
                    case BAM_CREF_SKIP:
                        /*
                        readStr << 'N';
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        curReadBase = ALN_REF_SKIP;
                        break;
                    case BAM_CSOFT_CLIP:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << 'S';
                        */
                        curRefBase = ALN_SOFT_CLIP;
                        break;
                    case BAM_CHARD_CLIP:
                        /*
                        readStr << 'H';
                        matchStr << ' ';
                        refStr << 'H';
                        */
                        curRefBase = ALN_HARD_CLIP;
                        curReadBase = ALN_HARD_CLIP;
                        break;
                    case BAM_CPAD:
                        /*
                        readStr << 'P';
                        matchStr << ' ';
                        refStr << 'P';
                        */
                        curRefBase = ALN_PAD;
                        curReadBase = ALN_PAD;
                        break;
                }

            curStateIdx = curRefBase * numStates + curReadBase;
            //stateStr << curStateIdx << ", ";
            double tp = transitionProbs[readPosBin](prevStateIdx, curStateIdx);
            logLike += tp;
            prevStateIdx = curStateIdx;
            if (BAM_CONSUME_SEQ(op)) {
                ++readIdx;
                curReadBase = samToTwoBit[bam_seqi(qseq, readIdx)];
                auto prev = readPosBin;
                readPosBin = static_cast<uint32_t>((readIdx * invLen));
            }
            if (BAM_CONSUME_REF(op)) {
                ++uTranscriptIdx;
                curRefBase = samToTwoBit[ref.baseAt(uTranscriptIdx, readStrand)];
            }

        }

        /*
        {
            std::lock_guard<std::mutex> l(outputMutex_);
            std::cerr << "\n\nread:   " << readStr.str() << "\n";
            std::cerr << "        " << matchStr.str() << "\n";
            std::cerr << "ref:    " << refStr.str() << "\n";
            std::cerr << "states: " << stateStr.str() << "\n";
            std::cerr << "prob:   " << std::exp(logLike) << "\n";
        }
        */
    }
    return logLike;
}

double AlignmentModel::logLikelihood(const ReadPair& hit, Transcript& ref){
    double logLike = sailfish::math::LOG_1;
    if (BOOST_UNLIKELY(!isEnabled_)) { return logLike; }

    if (!hit.isPaired()) {
        if (hit.isLeftOrphan()) {
            return logLikelihood(hit.read1, ref, transitionProbsLeft_);
        } else {
            return logLikelihood(hit.read1, ref, transitionProbsRight_);
        }
    }

    bam_seq_t* leftRead = (bam_pos(hit.read1) < bam_pos(hit.read2)) ? hit.read1 : hit.read2;
    bam_seq_t* rightRead = (bam_pos(hit.read1) < bam_pos(hit.read2)) ? hit.read2 : hit.read1;

    size_t leftLen = static_cast<size_t>(bam_seq_len(leftRead));
    size_t rightLen = static_cast<size_t>(bam_seq_len(rightRead));

    // NOTE: Raise a warning in this case?
    if (BOOST_UNLIKELY((leftLen > maxExpectedLen_) or
                       (rightLen > maxExpectedLen_))) {
        return logLike;
    }

    if (leftRead) {
        logLike += logLikelihood(leftRead, ref, transitionProbsLeft_);
    }

    if (rightRead) {
        logLike += logLikelihood(rightRead, ref, transitionProbsRight_);
    }
    if (logLike == sailfish::math::LOG_0) {
            std::lock_guard<std::mutex> lock(outputMutex_);
            std::cerr << "orphan status: " << hit.orphanStatus << "\n";
            std::cerr << "error likelihood: " << logLike << "\n";
    }

    return logLike;
}

double AlignmentModel::logLikelihood(const UnpairedRead& hit, Transcript& ref){
    double logLike = sailfish::math::LOG_1;
    if (BOOST_UNLIKELY(!isEnabled_)) { return logLike; }

    bam_seq_t* read = hit.read;
    size_t readLen = static_cast<size_t>(bam_seq_len(read));
    // NOTE: Raise a warning in this case?
    if (BOOST_UNLIKELY(readLen > maxExpectedLen_)) {
        return logLike;
    }
    logLike += logLikelihood(read, ref, transitionProbsLeft_);

    if (logLike == sailfish::math::LOG_0) {
            std::lock_guard<std::mutex> lock(outputMutex_);
            std::cerr << "error log likelihood: " << logLike << "\n";
    }

    return logLike;
}

void AlignmentModel::update(const UnpairedRead& hit, Transcript& ref, double p, double mass){
    if (mass == sailfish::math::LOG_0) { return; }
    if (BOOST_UNLIKELY(!isEnabled_)) { return; }
    bam_seq_t* leftRead = hit.read;
    update(leftRead, ref, p, mass, transitionProbsLeft_);
}

void AlignmentModel::update(bam_seq_t* read, Transcript& ref, double p, double mass,
                        std::vector<AtomicMatrix<double>>& transitionProbs) {
    using namespace sailfish::stringtools;
    bool useQual{false};
    size_t readIdx{0};
    auto transcriptIdx = bam_pos(read);
    size_t transcriptLen = ref.RefLength;
    // if the read starts before the beginning of the transcript,
    // only consider the part overlapping the transcript
    if (transcriptIdx < 0) {
        readIdx = -transcriptIdx;
        transcriptIdx = 0;
    }
    // unsigned version of transcriptIdx
    size_t uTranscriptIdx = static_cast<size_t>(transcriptIdx);

    uint32_t* cigar = bam_cigar(read);
    uint32_t cigarLen = bam_cigar_len(read);
    uint8_t* qseq = reinterpret_cast<uint8_t*>(bam_seq(read));
    uint8_t* qualStr = reinterpret_cast<uint8_t*>(bam_qual(read));

    /*
    std::stringstream readStr;
    std::stringstream matchStr;
    std::stringstream refStr;
    std::stringstream stateStr;
    */

    if (cigarLen > 0 and cigar) {

        sailfish::stringtools::strand readStrand = sailfish::stringtools::strand::forward;

        uint32_t readPosBin{0};
        uint32_t cigarIdx{0};
        uint32_t prevStateIdx{startStateIdx};
        uint32_t curStateIdx{0};
        double invLen = static_cast<double>(readBins_) / bam_seq_len(read);

        //stateStr << prevStateIdx << ", ";
        for (uint32_t cigarIdx = 0; cigarIdx < cigarLen; ++cigarIdx) {
            uint32_t opLen = cigar[cigarIdx] >> BAM_CIGAR_SHIFT;
            enum cigar_op op = static_cast<enum cigar_op>(cigar[cigarIdx] & BAM_CIGAR_MASK);
            size_t curReadBase = samToTwoBit[bam_seqi(qseq, readIdx)];
            size_t curRefBase = samToTwoBit[ref.baseAt(uTranscriptIdx, readStrand)];
            for (size_t i = 0; i < opLen; ++i) {
                switch (op) {
                    case BAM_UNKNOWN:
                        std::cerr << "ENCOUNTERED UNKNOWN SYMBOL IN CIGAR STRING!\n";
                        break;
                    case BAM_CMATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ((curReadBase == curRefBase) ? ' ' : 'X');
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CBASE_MATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CBASE_MISMATCH:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << 'X';
                        refStr << twoBitToChar[curRefBase];
                        */
                        // do nothing
                        break;
                    case BAM_CINS:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << '-';
                        */
                        curRefBase = ALN_DASH;
                        break;
                    case BAM_CDEL:
                        /*
                        readStr << '-';
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        curReadBase = ALN_DASH;
                        break;
                    case BAM_CREF_SKIP:
                        /*
                        readStr << 'N';
                        matchStr << ' ';
                        refStr << twoBitToChar[curRefBase];
                        */
                        curReadBase = ALN_REF_SKIP;
                        break;
                    case BAM_CSOFT_CLIP:
                        /*
                        readStr << twoBitToChar[curReadBase];
                        matchStr << ' ';
                        refStr << 'S';
                        */
                        curRefBase = ALN_SOFT_CLIP;
                        break;
                    case BAM_CHARD_CLIP:
                        /*
                        readStr << 'H';
                        matchStr << ' ';
                        refStr << 'H';
                        */
                        curRefBase = ALN_HARD_CLIP;
                        curReadBase = ALN_HARD_CLIP;
                        break;
                    case BAM_CPAD:
                        /*
                        readStr << 'P';
                        matchStr << ' ';
                        refStr << 'P';
                        */
                        curRefBase = ALN_PAD;
                        curReadBase = ALN_PAD;
                        break;
                }

                curStateIdx = curRefBase * numStates + curReadBase;

                //stateStr << curStateIdx << ", ";
                transitionProbs[readPosBin].increment(prevStateIdx, curStateIdx, mass+p);
                prevStateIdx = curStateIdx;
                if (BAM_CONSUME_SEQ(op)) {
                    ++readIdx;
                    curReadBase = samToTwoBit[bam_seqi(qseq, readIdx)];
                    readPosBin = static_cast<uint32_t>((readIdx * invLen));
                }
                if (BAM_CONSUME_REF(op)) {
                    ++uTranscriptIdx;
                    curRefBase = samToTwoBit[ref.baseAt(uTranscriptIdx, readStrand)];
                }
           }
        }

        /*
        {
            std::lock_guard<std::mutex> l(outputMutex_);
            std::cerr << "\n\nread:   " << readStr.str() << "\n";
            std::cerr << "        " << matchStr.str() << "\n";
            std::cerr << "ref:    " << refStr.str() << "\n";
            std::cerr << "states: " << stateStr.str() << "\n";
        }
        */
    } // if we had a cigar string
}

void AlignmentModel::update(const ReadPair& hit, Transcript& ref, double p, double mass){
    if (mass == sailfish::math::LOG_0) { return; }
    if (BOOST_UNLIKELY(!isEnabled_)) { return; }

    bam_seq_t* leftRead = (bam_pos(hit.read1) < bam_pos(hit.read2)) ? hit.read1 : hit.read2;
    bam_seq_t* rightRead = (bam_pos(hit.read1) < bam_pos(hit.read2)) ? hit.read2 : hit.read1;

    if (leftRead) {
        update(leftRead, ref, p, mass, transitionProbsLeft_);
    }

    if (rightRead) {
        update(rightRead, ref, p, mass, transitionProbsRight_);
    }
}
