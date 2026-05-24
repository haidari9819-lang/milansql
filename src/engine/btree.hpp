#pragma once

#include <string>
#include <vector>
#include <memory>

// ============================================================
// btree.hpp — B-Tree Index für MilanSQL
// Inspiriert von InnoDB B+ Tree Index-Struktur
//
// Ordnung T = 3:
//   Jeder Knoten (außer Root): min T-1=2, max 2T-1=5 Keys
//   Jeder innere Knoten:       min T=3,   max 2T=6  Kinder
//
// Key:   String (Spaltenwert)
// Value: std::vector<size_t> (Zeilenindizes — mehrere Treffer möglich)
//
// Beispiel Struktur mit 7 eingesetzten Keys:
//         [D]
//        l   r
//     [B]     [F]
//    +---+   +---+
//  [A] [C] [E] [G]
// ============================================================

namespace milansql {

class BTree {
public:
    static constexpr int T = 3;          // Mindestgrad
    static constexpr int MAX_KEYS = 2 * T - 1;  // = 5
    static constexpr int MIN_KEYS = T - 1;       // = 2

    // ── Knoten-Struktur ───────────────────────────────────────
    struct Node {
        bool isLeaf = true;

        // Parallel-Arrays: keys[i] gehört zu rowIndices[i]
        std::vector<std::string>           keys;
        std::vector<std::vector<size_t>>   rowIndices;

        // Kinder: children[i] liegt "links" von keys[i]
        // Nur für innere Knoten belegt (isLeaf == false)
        std::vector<std::unique_ptr<Node>> children;

        explicit Node(bool leaf = true) : isLeaf(leaf) {}

        bool isFull() const {
            return static_cast<int>(keys.size()) == MAX_KEYS;
        }
    };

    BTree() : root_(std::make_unique<Node>(true)) {}

    // Nicht kopierbar (wegen unique_ptr), aber bewegbar
    BTree(BTree&&)            = default;
    BTree& operator=(BTree&&) = default;
    BTree(const BTree&)       = delete;
    BTree& operator=(const BTree&) = delete;

    // ── Öffentliche API ───────────────────────────────────────

    // Schlüssel + Zeilenindex einfügen
    // Existiert der Schlüssel schon, wird rowIdx zum vorhandenen Eintrag addiert
    void insert(const std::string& key, size_t rowIdx) {
        Node* r = root_.get();

        if (r->isFull()) {
            // Root ist voll → neues Root anlegen, alte Root als erstes Kind
            auto newRoot = std::make_unique<Node>(false);
            newRoot->children.push_back(std::move(root_));
            root_ = std::move(newRoot);
            splitChild(root_.get(), 0);
        }
        insertNonFull(root_.get(), key, rowIdx);
    }

    // Suche: gibt alle Zeilenindizes für diesen Schlüssel zurück
    // Leerer Vektor = nicht gefunden
    std::vector<size_t> search(const std::string& key) const {
        return searchNode(root_.get(), key);
    }

    // Index leeren (für Rebuild nach UPDATE/DELETE)
    void clear() {
        root_ = std::make_unique<Node>(true);
    }

    // Anzahl gespeicherter Schlüssel (eindeutige Keys)
    size_t keyCount() const { return countKeys(root_.get()); }

private:
    std::unique_ptr<Node> root_;

    // ── SUCHE ─────────────────────────────────────────────────
    std::vector<size_t> searchNode(const Node* node,
                                   const std::string& key) const {
        int i = 0;
        int n = static_cast<int>(node->keys.size());

        // Kleinsten Index i finden mit keys[i] >= key
        while (i < n && key > node->keys[i]) ++i;

        // Exakter Treffer?
        if (i < n && key == node->keys[i]) {
            return node->rowIndices[i];
        }

        // Blatt ohne Treffer → nicht gefunden
        if (node->isLeaf) return {};

        // Im passenden Kind weitersuchen
        return searchNode(node->children[i].get(), key);
    }

    // ── EINFÜGEN ──────────────────────────────────────────────

    // Teilt das volle Kind children[i] eines nicht-vollen Elternknotens
    void splitChild(Node* parent, int i) {
        Node* y = parent->children[i].get();   // volles Kind
        auto  z = std::make_unique<Node>(y->isLeaf);

        // Mittlerer (Median) Key geht in den Elternknoten
        // Keys  0..T-2   bleiben in y  (T-1 Stück)
        // Key   T-1      geht hoch     (der Median)
        // Keys  T..2T-2  gehen in z    (T-1 Stück)

        // z erhält die oberen T-1 Keys
        for (int j = 0; j < T - 1; ++j) {
            z->keys.push_back(std::move(y->keys[T + j]));
            z->rowIndices.push_back(std::move(y->rowIndices[T + j]));
        }

        // Kinder aufteilen (nur bei inneren Knoten)
        if (!y->isLeaf) {
            for (int j = 0; j < T; ++j) {
                z->children.push_back(std::move(y->children[T + j]));
            }
            y->children.resize(T);
        }

        // Median-Key und zugehörige Zeilenindizes in Elternknoten heben
        std::string           medKey  = std::move(y->keys[T - 1]);
        std::vector<size_t>   medRows = std::move(y->rowIndices[T - 1]);

        // y auf T-1 Keys kürzen
        y->keys.resize(T - 1);
        y->rowIndices.resize(T - 1);

        // z als neues Kind in parent einfügen (rechts von y)
        parent->children.insert(parent->children.begin() + i + 1, std::move(z));

        // Median in parent einfügen
        parent->keys.insert(parent->keys.begin() + i, std::move(medKey));
        parent->rowIndices.insert(parent->rowIndices.begin() + i, std::move(medRows));
    }

    // Einfügen in einen garantiert nicht-vollen Knoten
    void insertNonFull(Node* node, const std::string& key, size_t rowIdx) {
        int i = static_cast<int>(node->keys.size()) - 1;

        if (node->isLeaf) {
            // ── Blattknoten: Duplikat-Check + sortiertes Einfügen ─
            for (int j = 0; j <= i; ++j) {
                if (node->keys[j] == key) {
                    // Schlüssel existiert → rowIdx anhängen
                    node->rowIndices[j].push_back(rowIdx);
                    return;
                }
            }
            // Platz schaffen und an richtiger Stelle einfügen
            node->keys.push_back({});
            node->rowIndices.push_back({});
            while (i >= 0 && key < node->keys[i]) {
                node->keys[i + 1]      = std::move(node->keys[i]);
                node->rowIndices[i + 1] = std::move(node->rowIndices[i]);
                --i;
            }
            node->keys[i + 1]      = key;
            node->rowIndices[i + 1] = {rowIdx};

        } else {
            // ── Innerer Knoten: passendes Kind finden ─────────
            while (i >= 0 && key < node->keys[i]) --i;

            // Schlüssel auf dieser Ebene gefunden? → rowIdx anhängen
            if (i >= 0 && node->keys[i] == key) {
                node->rowIndices[i].push_back(rowIdx);
                return;
            }

            ++i;   // i = Index des Kindes, in das wir absteigen

            // Kind bei Bedarf splitten, bevor wir absteigen
            if (node->children[i]->isFull()) {
                splitChild(node, i);
                // Nach dem Split: node->keys[i] ist der neue Median
                if      (key > node->keys[i]) ++i;
                else if (key == node->keys[i]) {
                    node->rowIndices[i].push_back(rowIdx);
                    return;
                }
            }
            insertNonFull(node->children[i].get(), key, rowIdx);
        }
    }

    // Hilfsfunktion: Keys zählen (für Debugging)
    static size_t countKeys(const Node* n) {
        size_t c = n->keys.size();
        for (const auto& child : n->children) c += countKeys(child.get());
        return c;
    }
};

} // namespace milansql
