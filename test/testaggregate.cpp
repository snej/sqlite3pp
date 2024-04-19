#include "test.h"
#include <string>
#include <iostream>
#include "sqnice/sqnice.hh"
#include "sqnice/ext.hh"

using namespace std;

void step0(sqnice::ext::context& c)
{
  int* sum = (int*) c.aggregate_data(sizeof(int));

  *sum += c.get<int>(0);
}
void finalize0(sqnice::ext::context& c)
{
  int* sum = (int*) c.aggregate_data(sizeof(int));
  c.result(*sum);
}

void step1(sqnice::ext::context& c)
{
  string* sum = (string*) c.aggregate_data(sizeof(string));

  if (c.aggregate_count() == 1) {
    new (sum) string;
  }

  *sum += c.get<string>(0);
}
void finalize1(sqnice::ext::context& c)
{
  string* sum = (string*) c.aggregate_data(sizeof(string));
  c.result(*sum);

  sum->~string();
}

template <class T>
struct mysum
{
  mysum() {
    s_ = T();
  }
  void step(T s) {
    s_ += s;
  }
  T finish() {
    return s_;
  }
  T s_;
};

struct mycnt
{
  void step() {
    ++n_;
  }
  int finish() {
    return n_;
  }
  int n_;
};

struct strcnt
{
  void step(string const& s) {
    s_ += s;
  }
  int finish() {
    return static_cast<int>(s_.size());
  }
  string s_;
};

struct plussum
{
  void step(int n1, int n2) {
    n_ += n1 + n2;
  }
  int finish() {
    return n_;
  }
  int n_;
};

int main_aggregate()
{
  try {
    sqnice::database db("foods.db");

    sqnice::ext::aggregate aggr(db);
    cout << aggr.create("a0", &step0, &finalize0) << endl;
    cout << aggr.create("a1", &step1, &finalize1) << endl;
    cout << aggr.create<mysum<string>, string>("a2") << endl;
    cout << aggr.create<mysum<int>, int>("a3") << endl;
    cout << aggr.create<mycnt>("a4") << endl;
    cout << aggr.create<strcnt, string>("a5") << endl;
    cout << aggr.create<plussum, int, int>("a6") << endl;

    sqnice::query qry(db, "SELECT a0(id), a1(name), a2(type_id), a3(id), a4(), a5(name), sum(type_id), a6(id, type_id) FROM foods");

    for (int i = 0; i < qry.column_count(); ++i) {
      cout << qry.column_name(i) << "\t";
    }
    cout << endl;

    for (sqnice::query::iterator i = qry.begin(); i != qry.end(); ++i) {
      for (int j = 0; j < qry.column_count(); ++j) {
	cout << (*i).get<char const*>(j) << "\t";
      }
      cout << endl;
    }
    cout << endl;
  }
  catch (exception& ex) {
    cout << ex.what() << endl;
  }
  return 0;
}
