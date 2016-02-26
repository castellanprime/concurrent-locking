#include <stdbool.h>

//======================================================

typedef TYPE QElem_t;

typedef struct CALIGN {
	QElem_t *elements;
	unsigned int front, rear;
} Queue;

static inline bool QnotEmpty( volatile Queue *queue ) {
	return queue->front != queue->rear;
} // QnotEmpty

static inline void Qenqueue( volatile Queue *queue, QElem_t element ) {
	queue->elements[queue->rear] = element;
	queue->rear = cycleUp( queue->rear, N );
} // Qenqueue

static inline QElem_t Qdequeue( volatile Queue *queue ) {
	QElem_t element = queue->elements[queue->front];
	queue->front = cycleUp( queue->front, N );
	return element;
} // Qdequeue

static inline void Qctor( Queue *queue ) {
	queue->front = queue->rear = 0;
	queue->elements = Allocator( N * sizeof(typeof(queue->elements[0])) );
} // Qctor

static inline void Qdtor( Queue *queue ) {
	free( queue->elements );
} // Qdtor

Queue queue CALIGN;

//======================================================

typedef struct CALIGN {									// performance gain when fields juxtaposed
	TYPE apply;
#ifdef FLAG
	TYPE flag;
#endif // FLAG
} Tstate;

static volatile TYPE fast CALIGN;
static volatile Tstate *tstate CALIGN;
//static volatile TYPE *apply CALIGN;
static volatile TYPE *val CALIGN;

#ifndef CAS
static volatile TYPE *b CALIGN, x CALIGN, y CALIGN;
#endif // ! CAS

#ifdef FLAG
//static volatile TYPE *fast CALIGN;
#else
static volatile TYPE lock CALIGN;
#endif // FLAG

static TYPE PAD CALIGN __attribute__(( unused ));		// protect further false sharing


#define await( E ) while ( ! (E) ) Pause()

#ifdef CAS

#define WCas( x ) __sync_bool_compare_and_swap( &(fast), false, true )

#else // ! CAS

#if defined( WCas1 )

static inline bool WCas( TYPE id ) {
	b[id] = true;
	x = id;
	Fence();											// force store before more loads
	if ( FASTPATH( y != N ) ) {
		b[id] = false;
		return false;
	} // if
	y = id;
	Fence();											// force store before more loads
	if ( FASTPATH( x != id ) ) {
		b[id] = false;
		Fence();										// force store before more loads
		for ( int j = 0; j < N; j += 1 )
			await( ! b[j] );
		if ( FASTPATH( y != id ) ) return false;
	} // if
	bool leader = ((! fast) ? fast = true : false);
	y = N;
	b[id] = false;
	return leader;
} // WCas

#elif defined( WCas2 )

static inline bool WCas( TYPE id ) {
	b[id] = true;
	Fence();											// force store before more loads
	for ( typeof(id) thr = 0; thr < id; thr += 1 ) {
		if ( FASTPATH( b[thr] ) ) {
			b[id] = false;
			return false ;
		} // if
	} // for
	for ( typeof(id) thr = id + 1; thr < N; thr += 1 ) {
		await( ! b[thr] );
	} // for
	bool leader = ((! fast) ? fast = true : false);
	b[id] = false;
	return leader;
} // WCas

#else
    #error unsupported architecture
#endif // WCas

#endif // CAS


static void *Worker( void *arg ) {
	TYPE id = (size_t)arg;
	uint64_t entry;

	const unsigned int n = N + id, dep = Log2( n );
	volatile typeof(tstate[0].apply) *applyId = &tstate[id].apply;
#ifdef FLAG
	volatile typeof(tstate[0].flag) *flagId = &tstate[id].flag;
	volatile typeof(tstate[0].flag) *flagN = &tstate[N].flag;
#endif // FLAG

#ifdef FAST
	unsigned int cnt = 0, oid = id;
#endif // FAST

	for ( int r = 0; r < RUNS; r += 1 ) {
		entry = 0;
		while ( stop == 0 ) {
			*applyId = true;
			// loop goes from parent of leaf to child of root
			for ( unsigned int j = (n >> 1); j > 1; j >>= 1 )
				val[j] = id;
#ifndef CAS
			Fence();									// force store before more loads
#endif // ! CAS

			if ( FASTPATH( WCas( id ) ) ) {
#ifndef CAS
				Fence();								// force store before more loads
#endif // ! CAS

#ifdef FLAG
				await( *flagN || *flagId );
				*flagN = false;
#else
				await( lock == N || lock == id );
				lock = id;
#endif // FLAG

				fast = false;
			} else {
#ifdef FLAG
				await( *flagId );
#else
				await( lock == id );
#endif // FLAG
			} // if
#ifdef FLAG
			*flagId = false;
#endif // FLAG
			*applyId = false;

			CriticalSection( id );

			// loop goes from child of root to leaf and inspects siblings
			for ( int j = dep - 1; j >= 0; j -= 1 ) { // must be "signed"
				typeof(val[0]) k = val[(n >> j) ^ 1];
				if ( FASTPATH( tstate[k].apply ) ) {
					tstate[k].apply = false;
					Qenqueue( &queue, k );
				} // if
			}  // for
			if ( FASTPATH( QnotEmpty( &queue ) ) ) {
#ifdef FLAG
				tstate[Qdequeue( &queue )].flag = true;
#else
				lock = Qdequeue( &queue );
#endif // FLAG
			} else
#ifdef FLAG
				*flagN = true;
#else
				lock = N;
#endif // FLAG

#ifdef FAST
			id = startpoint( cnt );						// different starting point each experiment
			cnt = cycleUp( cnt, NoStartPoints );
#endif // FAST
			entry += 1;
		} // while
#ifdef FAST
		id = oid;
#endif // FAST
		entries[r][id] = entry;
		__sync_fetch_and_add( &Arrived, 1 );
		while ( stop != 0 ) Pause();
		__sync_fetch_and_add( &Arrived, -1 );
	} // for
	return NULL;
} // Worker

void __attribute__((noinline)) ctor() {
	Qctor( &queue );
	tstate = Allocator( (N + 1) * sizeof(typeof(tstate[0])) );
	for ( TYPE id = 0; id <= N; id += 1 ) {				// initialize shared data
		tstate[id].apply = false;
#ifdef FLAG
		tstate[id].flag = false;
#endif // FLAG
	} // for

#ifdef FLAG
	tstate[N].flag = true;
#else
	lock = N;
#endif // FLAG

	val = Allocator( 2 * N * sizeof(typeof(val[0])) );
	for ( TYPE id = 0; id < N; id += 1 ) {				// initialize shared data
		val[id] = N;
		val[N + id] = id;
	} // for

#ifdef CAS
	fast = false;
#else
	b = Allocator( N * sizeof(typeof(b[0])) );
	for ( TYPE id = 0; id < N; id += 1 ) {				// initialize shared data
		b[id] = false;
	} // for
	y = N;
	fast = false;
#endif // CAS
} // ctor

void __attribute__((noinline)) dtor() {
#ifndef CAS
	free( (void *)b );
#endif // ! CAS

	free( (void *)val );
	free( (void *)tstate );
	Qdtor( &queue );
} // dtor

// Local Variables: //
// tab-width: 4 //
// compile-command: "gcc -Wall -std=gnu11 -O3 -DNDEBUG -fno-reorder-functions -DPIN -DAlgorithm=ElevatorQueue Harness.c -lpthread -lm" //
// End: //