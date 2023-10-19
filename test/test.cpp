#define BOOST_TEST_MODULE chainbase test
#include <boost/test/unit_test.hpp>
#include <chainbase/chainbase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

#include <iostream>
#include "temp_directory.hpp"
#include <thread>

using namespace chainbase;
using namespace boost::multi_index;

//BOOST_TEST_SUITE( serialization_tests, clean_database_fixture )

struct book : public chainbase::object<0, book> {

   template<typename Constructor, typename Allocator>
    book(  Constructor&& c, Allocator&& a ) {
       c(*this);
    }

    id_type id;
    int a = 0;
    int b = 1;
};

typedef multi_index_container<
  book,
  indexed_by<
     ordered_unique< member<book,book::id_type,&book::id> >,
     ordered_unique< BOOST_MULTI_INDEX_MEMBER(book,int,a) >,
     ordered_unique< BOOST_MULTI_INDEX_MEMBER(book,int,b) >
  >,
  chainbase::node_allocator<book>
> book_index;

CHAINBASE_SET_INDEX_TYPE( book, book_index )


BOOST_AUTO_TEST_CASE( open_and_create ) {
   temp_directory temp_dir;
   const auto& temp = temp_dir.path();
   std::cerr << temp << " \n";

   chainbase::database db(temp, database::read_write, 1024*1024*8, false, pinnable_mapped_file::map_mode::mapped);
   chainbase::database db2(temp, database::read_only, 0, true); /// open an already created db
   BOOST_CHECK_THROW( db2.add_index< book_index >(), std::runtime_error ); /// index does not exist in read only database

   db.add_index< book_index >();
   BOOST_CHECK_THROW( db.add_index<book_index>(), std::logic_error ); /// cannot add same index twice


   db2.add_index< book_index >(); /// index should exist now


   BOOST_TEST_MESSAGE( "Creating book" );
   const auto& new_book = db.create<book>( []( book& b ) {
      b.a = 3;
      b.b = 4;
   } );
   const auto& copy_new_book = db2.get( book::id_type(0) );
   BOOST_REQUIRE( &new_book != &copy_new_book ); ///< these are mapped to different address ranges

   BOOST_REQUIRE_EQUAL( new_book.a, copy_new_book.a );
   BOOST_REQUIRE_EQUAL( new_book.b, copy_new_book.b );

   db.modify( new_book, [&]( book& b ) {
      b.a = 5;
      b.b = 6;
   });
   BOOST_REQUIRE_EQUAL( new_book.a, 5 );
   BOOST_REQUIRE_EQUAL( new_book.b, 6 );

   BOOST_REQUIRE_EQUAL( new_book.a, copy_new_book.a );
   BOOST_REQUIRE_EQUAL( new_book.b, copy_new_book.b );

   {
      auto session = db.start_undo_session(true);
      db.modify( new_book, [&]( book& b ) {
         b.a = 7;
         b.b = 8;
      });

      BOOST_REQUIRE_EQUAL( new_book.a, 7 );
      BOOST_REQUIRE_EQUAL( new_book.b, 8 );
   }
   BOOST_REQUIRE_EQUAL( new_book.a, 5 );
   BOOST_REQUIRE_EQUAL( new_book.b, 6 );

   {
      auto session = db.start_undo_session(true);
      const auto& book2 = db.create<book>( [&]( book& b ) {
         b.a = 9;
         b.b = 10;
      });

      BOOST_REQUIRE_EQUAL( new_book.a, 5 );
      BOOST_REQUIRE_EQUAL( new_book.b, 6 );
      BOOST_REQUIRE_EQUAL( book2.a, 9 );
      BOOST_REQUIRE_EQUAL( book2.b, 10 );
   }
   BOOST_CHECK_THROW( db2.get( book::id_type(1) ), std::out_of_range );
   BOOST_REQUIRE_EQUAL( new_book.a, 5 );
   BOOST_REQUIRE_EQUAL( new_book.b, 6 );


   {
      auto session = db.start_undo_session(true);
      db.modify( new_book, [&]( book& b ) {
         b.a = 7;
         b.b = 8;
      });

      BOOST_REQUIRE_EQUAL( new_book.a, 7 );
      BOOST_REQUIRE_EQUAL( new_book.b, 8 );
      session.push();
   }
   BOOST_REQUIRE_EQUAL( new_book.a, 7 );
   BOOST_REQUIRE_EQUAL( new_book.b, 8 );
   db.undo();
   BOOST_REQUIRE_EQUAL( new_book.a, 5 );
   BOOST_REQUIRE_EQUAL( new_book.b, 6 );

   BOOST_REQUIRE_EQUAL( new_book.a, copy_new_book.a );
   BOOST_REQUIRE_EQUAL( new_book.b, copy_new_book.b );
}

BOOST_AUTO_TEST_CASE( oom_flush_dirty_pages ) {
   temp_directory temp_dir;
   const auto& temp = temp_dir.path();
   std::cerr << temp << " \n";

   constexpr size_t db_size = 4ull << 30; // 4GB
   constexpr size_t max_elems = db_size / (sizeof(book) + 16 + 4);
   chainbase::database db(temp, database::read_write, db_size, false, pinnable_mapped_file::map_mode::mapped_private);
   db.add_index< book_index >();

   auto& pmf = db.get_pinnable_mapped_file();
   pmf.set_oom_threshold(100); // set a low threshold so that we hit it with a 4GB file
   pmf.set_oom_delay(0);

   size_t flush_count = 0;
   for (size_t i=0; i<max_elems; ++i) {
      db.create<book>( [i]( book& b ) { b.a = (int)i; b.b = (int)(i+1); } );
      if (i % 1000 == 0) {
         if (auto res = db.check_memory_and_flush_if_needed()) {
            std::cerr << "oom score: " << res->oom_score_before << '\n';
            if (res->num_pages_written > 0) {
               std::cerr << "Flushed " << res->num_pages_written << " pages to disk\n";
               if (++flush_count == 6)
                  break;
            }
         }

         // check that we still have all previously created items
         for (size_t k=0; k<i; ++k) {
            const auto& book = db.get( book::id_type(k) );
            BOOST_REQUIRE_EQUAL( book.a, (int)k );
            BOOST_REQUIRE_EQUAL( book.b, (int)(k+1) );
         }
         
      }
      BOOST_REQUIRE_EQUAL( db.get_index<get_index_type<book>::type>().size(), i+1);
      BOOST_REQUIRE(db.get_index<get_index_type<book>::type>().size() == i+1);
      const auto& last_inserted_book = db.get( book::id_type(i) );
      BOOST_REQUIRE_EQUAL( last_inserted_book.a, (int)i );
      BOOST_REQUIRE_EQUAL( last_inserted_book.b, (int)(i+1) );

   }
   BOOST_REQUIRE(flush_count == 6); 
}

// BOOST_AUTO_TEST_SUITE_END()
