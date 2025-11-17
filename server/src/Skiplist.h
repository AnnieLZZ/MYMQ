#ifndef SKIPLIST_H
#define SKIPLIST_H
#include <vector>
#include <random>
#include <cassert>

template<typename T>
class Skiplist {
private:
    struct Node {
        T value;
        std::vector<Node*> forward;
        Node(T val, size_t level) : value(val), forward(level, nullptr) {}
    };

    Node* head;
    Node* tail;
    int maxLevel;
    int size;
    std::mt19937 gen;
    std::uniform_real_distribution<> dis;

    int randomLevel() {
        int level = 1;
        while (dis(gen) < 0.5 && level < maxLevel) {
            level++;
        }
        return level;
    }

public:
    Skiplist(int maxLevel = 16) : maxLevel(maxLevel), size(0), gen(std::random_device{}()), dis(0.0, 1.0) {
        head = new Node(T(), maxLevel);
        tail = nullptr;
    }

    ~Skiplist() {
        Node* current = head->forward[0];
        while (current != nullptr) {
            Node* next = current->forward[0];
            delete current;
            current = next;
        }
        delete head;
    }

    bool empty() const {
        return size == 0;
    }



    void clear() {
        Node* current = head->forward[0];
        while (current != nullptr) {
            Node* next = current->forward[0];
            delete current;
            current = next;
        }


        for (int i = 0; i < maxLevel; ++i) {
            head->forward[i] = nullptr;
        }

        tail = nullptr;
        size = 0;
    }


    void insert(const T& value) {
        std::vector<Node*> update(maxLevel, nullptr);
        Node* current = head;

        for (int i = maxLevel - 1; i >= 0; --i) {
            while (current->forward[i] != nullptr && current->forward[i]->value < value) {
                current = current->forward[i];
            }
            update[i] = current;
        }

        int level = randomLevel();
        Node* newNode = new Node(value, level);

        for (int i = 0; i < level; ++i) {
            if (i < maxLevel) {
                newNode->forward[i] = update[i]->forward[i];
                update[i]->forward[i] = newNode;
            }
        }

        if (newNode->forward[0] == nullptr) {
            tail = newNode;
        }

        size++;
    }

    bool contains(const T& value) const {
        Node* current = head;
        for (int i = maxLevel - 1; i >= 0; --i) {
            while (current->forward[i] != nullptr && current->forward[i]->value < value) {
                current = current->forward[i];
            }
        }
        current = current->forward[0];
        return current != nullptr && current->value == value;
    }




    class iterator {
    private:
        Node* ptr;

    public:
        iterator(Node* p = nullptr) : ptr(p) {}

        T& operator*() const {
            return ptr->value;
        }

        iterator& operator++() {
            if (ptr) {
                ptr = ptr->forward[0];
            }
            return *this;
        }

        iterator operator++(int) {
            iterator temp = *this;
            ++(*this);
            return temp;
        }

        bool operator!=(const iterator& other) const {
            return ptr != other.ptr;
        }

        bool operator==(const iterator& other) const {
            return ptr == other.ptr;
        }
    };

    iterator begin() {
        return iterator(head->forward[0]);
    }

    iterator end() {
        return iterator(nullptr);
    }

    iterator rbegin() {
        return iterator(tail);
    }

    iterator rend() {
        return iterator(nullptr);
    }
};


#endif // SKIPLIST_H
