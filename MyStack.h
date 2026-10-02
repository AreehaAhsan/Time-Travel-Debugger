#pragma once
# ifndef MYSTACK_H
# define MYSTACK_H
#include<iostream>
#include<vector>
#include <exception>

using namespace std;

template <typename T>

class MyStack {

	vector<T> st;

public:
	void push(T val) {
		st.push_back(val);
	}

	T top() {
		if (!st.empty()) {

			return st[(st.size() - 1)];

		}

		throw underflow_error("Stack is empty.");

	}

	void pop(){

		if (!st.empty()) {
			st.pop_back();
			return;

		}

		throw underflow_error("Stack is empty.");

	}

	bool isEmpty(){
		return st.empty();
	}

};

#endif