#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/mman.h>

#ifdef NUMA
    #include <numa.h>
    #include <numaif.h>
    #include <sched.h>
#endif


//bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval)


#define LEAF_FULL_MASK (unsigned long) (0x1F)
#define COAL_LEFT  ((unsigned long)(0x8))
#define COAL_RIGHT  ((unsigned long)(0x4))
#define LOCK_LEAF ((unsigned long)(0x13))
#define TOTAL ((unsigned long)(0xffffffffffffffff))
#define ABORT TOTAL
#define LOCK_LEAF_MASK (unsigned long) (0x13)
#define LOCK_NOT_LEAF_MASK (unsigned long) (0x1)


//questo è 8! quindi qua devo proprio vedere con le pos dal nodo.. parte da 1!
#define LEAF_START_POSITION (8) //la prima foglia del grappolo.. dipende dalla grandezza dei grappoli e si riferisce al node_container

#define LEFT  ((unsigned long)(0x2))
#define RIGHT  ((unsigned long)(0x1))


#define IS_FREE(val, pos)  !IS_OCCUPIED(val, pos)
#define IS_LEAF(n) (((n)->container_pos) >= (LEAF_START_POSITION)) //attenzione: questo ti dice se il figlio è tra le posizione 8-15. Se sei foglia di un grappolo piccolo qua non lo vedi
#define IS_BUNCHROOT(n) ( (n->container_pos) == (1) )
#define BUNCHROOT(n) ((n->container->bunch_root))
#define LOCK_NOT_A_LEAF(val, pos)  ((val) | (LOCK_NOT_LEAF_MASK << ((pos-1))))
#define LOCK_A_LEAF(val, pos) ((val) | (LOCK_LEAF_MASK << ((LEAF_START_POSITION-1) + (5 * ( (pos-1) - (LEAF_START_POSITION-1))))))
#define UNLOCK_NOT_A_LEAF(val, pos)  ((val) & (~ (LOCK_NOT_LEAF_MASK << ((pos-1)))))
#define UNLOCK_A_LEAF(val, pos) ((val) & (~ (LOCK_LEAF_MASK << ((LEAF_START_POSITION-1) + (5 * ((pos-1) - (LEAF_START_POSITION-1)))))))


//NOTA CHE QUESTE DEL COALESCE E LEFT/RIGHT SI DEVONO USA SOLO SULLE FOGLIE
#define COALESCE_LEFT(NODES, pos) (unsigned long) (NODES | (((COAL_LEFT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - LEAF_START_POSITION))))))
#define COALESCE_RIGHT(NODES, pos) (unsigned long) (NODES | (((COAL_RIGHT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - LEAF_START_POSITION))))))

#define CLEAN_LEFT_COALESCE(NODES,pos) (unsigned long) (NODES &  (~((COAL_LEFT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - LEAF_START_POSITION))))))
#define CLEAN_RIGHT_COALESCE(NODES,pos) (unsigned long)  (NODES &  (~((COAL_RIGHT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - LEAF_START_POSITION))))))

#define OCCUPY_LEFT(NODES,pos)  (unsigned long) ((NODES) | ((LEFT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - (LEAF_START_POSITION))))))
#define OCCUPY_RIGHT(NODES,pos) (unsigned long)  ((NODES) | ((RIGHT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - (LEAF_START_POSITION))))))

#define CLEAN_LEFT(NODES, pos) (unsigned long) ((NODES) &(~ ((LEFT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - (LEAF_START_POSITION)))))))
#define CLEAN_RIGHT(NODES, pos) (unsigned long) ((NODES) &(~ ((RIGHT) << ((LEAF_START_POSITION-1) + (5 * ((pos) - (LEAF_START_POSITION)))))))

#define IS_OCCUPIED_LEFT(NODES, POS)  ((NODES) == (OCCUPY_LEFT(NODES,POS)))
#define IS_OCCUPIED_RIGHT(NODES, POS)  ((NODES) == (OCCUPY_RIGHT(NODES,POS)))

#define IS_COALESCING_LEFT(NODES, POS) ((NODES) == (COALESCE_LEFT(NODES,POS)))
#define IS_COALESCING_RIGHT(NODES, POS)  ((NODES) == ( COALESCE_RIGHT(NODES,POS)) )

//se il fratello è in uso non devo risalire più a smarcare! se lo faccio sblocco il genitore di mio fratello che è occupato!
#define CHECK_BROTHER(parent, current, actual_value) \
        { \
            if((&left(parent))==current && (!IS_ALLOCABLE(actual_value,(&right(parent))->container_pos))){\
                exit=true;\
                break;\
        }\
            else if(&(right(parent))==current && (!IS_ALLOCABLE(actual_value,(&left(parent))->container_pos))){\
                exit=true;\
                break;\
            }\
        }


#define VAL_OF_NODE(n) ((unsigned long) (n->container_pos<LEAF_START_POSITION ) ? ((n->container->nodes & (0x1 << (n->container_pos-1))) >> (n->container_pos-1)) : ((n->container->nodes & (LEAF_FULL_MASK << ((LEAF_START_POSITION-1) + (5 * ((n->container_pos-1) - (LEAF_START_POSITION-1))))))) >> ((LEAF_START_POSITION-1) + (5 * ((n->container_pos-1) - (LEAF_START_POSITION-1)))))

#define ROOT (tree[1])
#define left(n) (tree[((n->pos)*(2))])
#define right(n) (tree[(((n->pos)*(2))+(1))])
#define left_index(n) (((n)->pos)*(2))
#define right_index(n) ((((n)->pos)*(2))+(1))
#define parent(n) (tree[(unsigned)(((n)->pos)/(2))])
#define parent_index(n) ((unsigned)(((n)->pos)/(2)))
#define level(n) ((unsigned) ( (overall_height) - (log2_(( (n)->mem_size) /(PAGE_SIZE) )) ))
#define MIN_ALLOCABLE_PAGES 1 //numero minimo di pagine allocabili
#define MAX_ALLOCABLE_PAGES ((1024 < overall_memory_pages) ? (1024): (overall_memory_pages))  //1024 pages = 4MB. Per gestire 1GB servono 262.144 pages
#define PAGE_SIZE (4096)
#define LEVEL_PER_CONTAINER 4

typedef struct _node node;

typedef struct node_container_{
    unsigned long nodes;
    node* bunch_root;
}node_container;

struct _node{
    unsigned long mem_start; //spiazzamento all'interno dell'overall_memory
    unsigned long mem_size;
    node_container* container;
    unsigned pos; //posizione all'interno dell'array "tree"
#ifdef NUMA
    unsigned numa_node; //to remember to which tree the node belongs to
#endif
    char container_pos; //posizione all'interno del rispettivo container (1-15)
};

unsigned master;

#ifdef NUMA
    bool allocate_in_remote_node = true; //make it possible to allocate in remote no. set to false if you don't want.
    node** overall_trees; //array di alberi! Un albero per ognu NUMA node
#endif
node* tree; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1].
unsigned long overall_memory_size;
unsigned long overall_memory_pages;
unsigned number_of_nodes; //questo non tiene presente che tree[0] è dummy! se qua c'è scritto 7 vuol dire che ci sono 7 nodi UTILIZZABILI
#ifdef NUMA
    void** overall_memory; //array of pointers. One pointer for each numa NODE
#else
    void* overall_memory;
#endif
node trying;
unsigned failed_at_node;
unsigned overall_height;
unsigned mypid;

#ifdef NUMA
    node_container** overall_containers; //Uno per ogni albero (uno per ogni nodo)
#endif
node_container* containers; //array di container In Numa questo sarà uno specifico tree (di base tree[getMyNUMANode])

unsigned first_available_container = 0;
node* upper_bound;

unsigned int* processes_done; //per scrivere cose coerenti, si aspettano
int number_of_processes;
#ifdef NUMA
int number_of_numa_nodes;
#endif


typedef struct _taken_list_elem{
    struct _taken_list_elem* next;
    node* elem;
}taken_list_elem;

typedef struct _taken_list{
    struct _taken_list_elem* head;
    unsigned number;
}taken_list;


taken_list* takenn;


unsigned long upper_power_of_two(unsigned long v);
void init_node(node* n);
void init_tree(unsigned long number_of_nodes);
void init(unsigned long memory);
void free_nodes(node* n); //questo fa la free fisicamente
void end();
void print(node* n);
bool alloc(node* n);
void marca(node* n);
bool IS_OCCUPIED(unsigned long, unsigned);
#ifdef NUMA
void do_move(void * buffer, unsigned target_node, unsigned n_pages);
#endif


bool check_parent(node* n);
void smarca_(node* n);

void find_the_bug_on_new_val(unsigned long new_val){
    unsigned long val[15];
    val[0] = (new_val & ((unsigned long) (0x1)));
    val[1] = (new_val & ((unsigned long) (0x1<<1))) >> 1;
    val[2] = (new_val & ((unsigned long) (0x1<<2))) >> 2;
    val[3] = (new_val & ((unsigned long) (0x1<<3))) >> 3;
    val[4] = (new_val & ((unsigned long) (0x1<<4))) >> 4;
    val[5] = (new_val & ((unsigned long) (0x1<<5)))>> 5;
    val[6] = (new_val & ((unsigned long) (0x1<<6)))>> 6;
    val[7] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((8-1) - (7)))))) >> ((7) + (5 * ((8-1) - (7))));
    val[8] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((9-1) - (7)))))) >> ((7) + (5 * ((9-1) - (7))));
    val[9] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((10-1) - (7)))))) >> ((7) + (5 * ((10-1) - (7))));
    val[10] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((11-1) - (7)))))) >> ((7) + (5 * ((11-1) - (7))));
    val[11] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((12-1) - (7)))))) >> ((7) + (5 * ((12-1) - (7))));
    val[12] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((13-1) - (7)))))) >> ((7) + (5 * ((13-1) - (7))));
    val[13] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((14-1) - (7)))))) >> ((7) + (5 * ((14-1) - (7))));
    val[14] = (new_val & (LEAF_FULL_MASK << ((7) + (5 * ((15-1) - (7)))))) >> ((7) + (5 * ((15-1) - (7))));
    int i;
    for(i=0;i<15;i++){
        if(val[i]!=1 && val[i]!=2 && val[i]!=10 && val[i]!=5 && val[i]!=19 && val[i]!=0 && val[i]!=15 && val[i]!=3 && val[i]!=11 && val[i]!=7) {
            printf("\n è uscito: %lu!!!!!\n", val[i]);
            abort();
        }
    }
}

void  find_the_bug(int who){
    int i;
    
    for(i=1;i<=number_of_nodes;i++){
        node* n = &tree[i];
        unsigned long val = VAL_OF_NODE(n);
        if(val!=1 && val!=2 && val!=10 && val!=5 && val!=19 && val!=0 && val!=15 && val!=3 && val!=11 && val!=7){
            if(who==0)
                printf("è stata la alloc e ha detto %lu\n", val);
            else if(who==1)
                printf("è stata la free e ha detto %lu\n", val);
            else if(who==2)
                printf("è stata proprio la free e ha detto %lu\n", val);
            else if(who==3)
                printf ("è stata la smarca e ha detto %lu\n", val);
            else if(who==4)
                printf("è stata la marca e ha detto %lu\n", val);
            else if(who==5)
                printf("è stata la free_node con 5 e ha detto %lu\n", val);
            abort();
        }
    }
    
}

void smarca(node* n){
#ifdef NUMA
    tree = overall_trees[n->numa_node];
    containers = overall_containers[n->numa_node];
#endif
    upper_bound = &ROOT;
    smarca_(n);
}

void print_in_profondita(node*);
void print_in_ampiezza();
void free_node_(node* n);

/*Queste funzioni sono esposte all'utente*/
node* request_memory(unsigned pages);
void free_node(node* n){
#ifdef NUMA
    tree = overall_trees[n->numa_node];
    containers = overall_containers[n->numa_node];
#endif
    upper_bound = &ROOT;
    free_node_(n);
}

unsigned log2_(unsigned long value);

//MARK: WRITE SU FILE

/*
    SCRIVE SU FILE I NODI PRESI DA UN THREAD - FUNZIONE PRETTAMENTE DI DEBUG
 */
void write_taken(){
    char filename[128];
    sprintf(filename, "./debug/taken_%d.txt", getpid()%number_of_processes);
    FILE *f = fopen(filename, "w");
    unsigned i;
    
    if (f == NULL){
        printf("Error opening file!\n");
        exit(1);
    }
    
    taken_list_elem* runner = takenn->head;
    
    /* print some text */
    for(i=0;i<takenn->number;i++){
        fprintf(f, "%u\n", runner->elem->pos);
        runner=runner->next;
    }
    
    
    
    fclose(f);
    
}



/* 
    SCRIVE SU FILE LA SITUAZIONE DELL'ALBERO (IN AMPIEZZA) VISTA DA UN CERTO THREAD
 */

void write_on_a_file_in_ampiezza(){
    char filename[128];
    int i;
#ifdef NUMA
    int actual_node;
    for(actual_node=0;actual_node<number_of_numa_nodes;actual_node++){
        tree = overall_trees[actual_node];
        //puts("stampo");
        sprintf(filename, "./debug/tree%d_%d.txt", actual_node, getpid());
        FILE *f = fopen(filename, "w");
        
        if (f == NULL){
            printf("Error opening file!\n");
            exit(1);
        }
        fprintf(f, "%d \n", getpid());
        
        for(i=1;i<=number_of_nodes;i++){
            node* n = &tree[i];
            fprintf(f, "(%p) %u val=%lu has %lu B. mem_start in %lu  level is %u\n", (void*)n, tree[i].pos,  VAL_OF_NODE(n) , tree[i].mem_size, tree[i].mem_start,  level(n));
        }
        
        fclose(f);
    }
    
#else
    if(getpid()==master)
        sprintf(filename, "./debug/tree.txt");
    else
        sprintf(filename, "./debug/tree2.txt");
    FILE *f = fopen(filename, "w");
    
    if (f == NULL){
        printf("Error opening file!\n");
        exit(1);
    }
    fprintf(f, "%d \n", getpid());
    
    for(i=1;i<=number_of_nodes;i++){
        node* n = &tree[i];
        fprintf(f, "%d: (%p) %u val=%lu has %lu B. mem_start in %lu  level is %u\n", getpid(), (void*)n, tree[i].pos,  VAL_OF_NODE(n) , tree[i].mem_size, tree[i].mem_start,  level(n));
    }
    
    fclose(f);
#endif
}

void write_on_a_file_touched(){
    char filename[128];
    sprintf(filename, "./debug/touched.txt");
    FILE *f = fopen(filename, "w");
    int i;
    
    if (f == NULL){
        printf("Error opening file!\n");
        exit(1);
    }
    
    for(i=1;i<=number_of_nodes;i++){
        node* n = &tree[i];
        if(VAL_OF_NODE(n)!=0){
            fprintf(f,"%u\n", n->pos);
        }
    }
    
    fclose(f);
}


//MARK: MATHS
/*
    CALCOLA LA POTENZA DI DUE
 */


//Random limitato da limit [0,limit].. estremi inclusi
unsigned rand_lim(unsigned limit) {
    /* return a random number between 0 and limit inclusive.
     */
    int divisor = RAND_MAX/(limit+1);
    int retval;

    do {
        retval = rand() / divisor;
    } while (retval > limit);
    
    
    return retval;
}

unsigned long upper_power_of_two(unsigned long v){
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++;
    return v;
}


/*log2 malato*/
const unsigned tab64[64] = {
    63,  0, 58,  1, 59, 47, 53,  2,
    60, 39, 48, 27, 54, 33, 42,  3,
    61, 51, 37, 40, 49, 18, 28, 20,
    55, 30, 34, 11, 43, 14, 22,  4,
    62, 57, 46, 52, 38, 26, 32, 41,
    50, 36, 17, 19, 29, 10, 13, 21,
    56, 45, 25, 31, 35, 16,  9, 12,
    44, 24, 15,  8, 23,  7,  6,  5};

unsigned log2_ (unsigned long value){
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    return tab64[((unsigned long)((value - (value >> 1))*0x07EDD5E59A4E28C2)) >> 58];
}

//MARK: INIT


/*
 Questa funzione inizializza l'albero. Non il nodo dummy (tree[0])
 @param number_of_nodes: the number of nodes.
 */
void init_tree(unsigned long number_of_nodes){
    int i=0;
#ifdef NUMA
    int j = -1;
next_node_label:
//    puts("giro");
    first_available_container = 0;
    j++;
    tree = overall_trees[j];
    containers = overall_containers[j];
#endif

    ROOT.mem_start = 0;
    ROOT.mem_size = overall_memory_size;
    ROOT.pos = 1;
    ROOT.container = &containers[first_available_container++];
    ROOT.container_pos = 1;
    ROOT.container->bunch_root = &ROOT;
#ifdef NUMA
    ROOT.numa_node = j;
#endif
    
    //init_node(ROOT);
    for(i=2;i<=number_of_nodes;i++){
#ifdef AUDIT
        printf("%d\n", i);
#endif
        tree[i].pos = i;
        node parent = parent(&tree[i]);
        tree[i].mem_size = (parent.mem_size) / 2;
#ifdef NUMA
        tree[i].numa_node = j;
#endif
        
        if(level(&tree[i])%LEVEL_PER_CONTAINER==1){
            tree[i].container = &containers[first_available_container++];
            tree[i].container_pos = 1;
            tree[i].container->bunch_root = &tree[i];
        }
        else{
            tree[i].container = parent.container;
            if(left_index(&parent)==i)
                tree[i].container_pos = parent.container_pos*2;
            
            else
                tree[i].container_pos = (parent.container_pos*2)+1;
            
        }
            
        
        if(left_index(&parent)==i)
            tree[i].mem_start = parent.mem_start;
        
        else
            tree[i].mem_start = parent.mem_start + tree[i].mem_size;
        
       
    }

#ifdef NUMA
    if(j != number_of_numa_nodes-1)
        goto next_node_label;
#endif
    
    
#ifdef AUDIT
    for(i=1;i<=number_of_nodes;i++){
        printf("I am %u, my container is %ld and my position is %u level is %u\n", tree[i].pos, tree[i].container - containers, tree[i].container_pos, level(&tree[i]));
    }
    exit(0);
    
#endif
    
    //exit(0);
}

#ifdef NUMA
/*
    Funzione che effettua la move_pages (portando le pagine sul nodo target_node) delle n_pages che iniziano da buffer.
    @param buffer: puntatore all'inizio della memoria da migrare
    @param target_node: nodo su cui spostare la memoria
    @param n_pages: numero di pagine da spostare partendo da buffer

 */
void do_move(void * buffer, unsigned target_node, unsigned n_pages) {
#ifdef NUMA_AUDIT
    printf("******MIGRATE TO %u\n", target_node);
#endif
    void* pages[n_pages];
    int nodes[n_pages];
    int status[n_pages];
    int i;
    int returned;
    for (i = 0; i < n_pages; i++){
        pages[i] = buffer + (i * PAGE_SIZE);
        nodes[i] = target_node;
        status[i] = -1;
    }
    for (i = 0; i < n_pages; i++){
        //INITIALIZE
        *(long*)pages[i] = 0;
    }

    returned = move_pages(0, n_pages, (void **)pages, nodes, status, MPOL_MF_MOVE);
    if(returned != 0)
        puts("something was wrong in move_pages..");
#ifdef NUMA_AUDIT
    printf("returned is %d, errno is%d\n", returned, errno);
    for (i = 0; i < n_pages; i++){
        printf("status[%d]=%d\n", i, status[i]);
    }
#endif
}
#endif


#ifdef NUMA
/*
	@param pages: pagine richieste. Questo sarà il valore della radice - NUMA version
 */
void init(unsigned long pages){
    
#ifdef NUMA_AUDIT
    int numa_node = -1;
#endif
    
    overall_memory_pages = upper_power_of_two(pages);
    overall_memory_size =  overall_memory_pages * PAGE_SIZE;
    
    int i;
    int num_pages;
    //se abbiamo numa overall_memory è un array di puntatori. Ognuno è presente in un certo nodo.
    overall_memory = mmap(NULL, number_of_numa_nodes * sizeof(void*), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(overall_memory==MAP_FAILED)
        abort();
    for(i=0;i<number_of_numa_nodes;i++){
        //overall_memory[i] = numa_alloc_onnode(overall_memory_size, i);
        overall_memory[i] = mmap(NULL, overall_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        num_pages = overall_memory_size / PAGE_SIZE;
        if((overall_memory_size % PAGE_SIZE) != 0)
            num_pages++;
        do_move((void*)overall_memory[i], i, num_pages);
        if(overall_memory[i]==NULL)
            abort();
#ifdef NUMA_AUDIT
        numa_node = -1;
        get_mempolicy(&numa_node, NULL, 0, (void*)overall_memory[i], MPOL_F_NODE | MPOL_F_ADDR);
        printf("overall_memory[i] (expected NODE) is %d, numa_node is %d\n", i, numa_node);
#endif
    }
    
    overall_height = log2_(overall_memory_pages)+1;
    number_of_nodes = overall_memory_pages*2-1;
    
    
    //se abbiamo numa tree è un array  e containers, sono array, ognuno riferito ad un certo numa node
    overall_trees = mmap(NULL, number_of_numa_nodes * sizeof(node*), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(overall_trees==MAP_FAILED)
        abort();
    
    overall_containers = mmap(NULL, number_of_numa_nodes * sizeof(node_container*), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(overall_containers==MAP_FAILED)
        abort();
    for(i=0;i<number_of_numa_nodes;i++){
        overall_trees[i] = mmap(NULL,(1+number_of_nodes)*sizeof(node), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        overall_containers[i] = mmap(NULL,(number_of_nodes-1)*sizeof(node_container),PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        
        num_pages = ((1+number_of_nodes)*sizeof(node)) / PAGE_SIZE;
        if((((1+number_of_nodes)*sizeof(node)) % PAGE_SIZE) != 0)
            num_pages++;
        do_move((void*)overall_trees[i], i, num_pages);
        
        num_pages = ((number_of_nodes-1)*sizeof(node_container))/PAGE_SIZE;
        if((((number_of_nodes-1)*sizeof(node_container)) % PAGE_SIZE) != 0)
            num_pages++;
        do_move((void*)overall_containers[i], i, num_pages);
        
        if(overall_trees[i]==NULL || overall_containers[i]==NULL)
            abort();
        
        
        
#ifdef NUMA_AUDIT
        numa_node = -1;
        get_mempolicy(&numa_node, NULL, 0, (void*)overall_trees[i], MPOL_F_NODE | MPOL_F_ADDR);
        printf("overall_trees[i] (expected NODE) is %d, numa_node is %d\n", i, numa_node);
        numa_node = -1;
        get_mempolicy(&numa_node, NULL, 0, (void*)overall_containers[i], MPOL_F_NODE | MPOL_F_ADDR);
        printf("overall_containers[i] (expected NODE) is %d, numa_node is %d\n", i, numa_node);
#endif
    }
    
    init_tree(number_of_nodes);
    puts("NUMA init complete");
    
}
#else


/*
	@param pages: pagine richieste. Questo sarà il valore della radice - NON NUMA VERSION
 */
void init(unsigned long pages){
    overall_memory_pages = upper_power_of_two(pages);
    overall_memory_size =  overall_memory_pages * PAGE_SIZE;
    
    overall_memory = mmap(NULL, overall_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
//   printf("overall memory is %p of size %lu\n", overall_memory, overall_memory_size);
    
    if(overall_memory==MAP_FAILED)
        abort();
    
    overall_height = log2_(overall_memory_pages)+1;
    number_of_nodes = overall_memory_pages*2-1;
    

    tree = mmap(NULL,(1+number_of_nodes)*sizeof(node), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(tree==MAP_FAILED)
        abort();
    
    containers = mmap(NULL,(number_of_nodes-1)*sizeof(node_container), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(containers==MAP_FAILED)
        abort();
    
    init_tree(number_of_nodes);
    puts("UMA init complete");
    
}
#endif

/*
    Funzione finale che nell'ordine:
    1) libera la memoria assegnabile
    3) libera l'array che memorizzava l'albero
 */
void end(){
#ifdef NUMA
    int i;
    for(i=0;i<number_of_numa_nodes;i++){
        free(overall_memory[i]);
        free(overall_containers[i]);
        free(overall_trees[i]);
    }
    free(overall_memory);
    free(overall_containers);
    free(overall_trees);
    
#else
    free(overall_memory);
    //free_nodes(ROOT);
    free(tree);
#endif
}

//MARK: PRINT


/*traversal tramite left and right*/

void print_in_profondita(node* n){
    printf("%u has\n", n->pos);
    printf("%u has %lu B. mem_start in %lu left is %u right is %u status=%lu level=%u\n", n->pos, n->mem_size, n->mem_start, left_index(n), right_index(n), VAL_OF_NODE(n), level(n));
    if(left_index(n)<= number_of_nodes){
        print_in_profondita(&left(n));
        print_in_profondita(&right(n));
    }
}

/*Print in ampiezza*/

void print_in_ampiezza(){
    int i;
    for(i=1;i<=number_of_nodes;i++){
        node* n = &tree[i];
        //per il momento stampiano la stringa
        printf("%u val=%lu has %lu B. mem_start in %lu  level is %u\n", tree[i].pos,  VAL_OF_NODE(n) , tree[i].mem_size, tree[i].mem_start,  level(n));
        
        
    }
}



//MARK: ALLOCAZIONE

/*
    Questa funziona controlla se il nodo in posizione container_pos è libero nella maschera dei nodi rappresentata da val. Per la semantica dell'algoritmo un nodo è allocabile se e solo se esso è completamente libero, i.e. il nodo è completamento 0. Per questo motivo per le foglie il check è fatto con 0x1F
    @param val: il container nel quale il nodo si trova
    @param pos: il container_pos del nodo che vogliamo controllare
 
 */
bool IS_ALLOCABLE(unsigned long val, unsigned pos){
    bool ret = false;
    if(pos<LEAF_START_POSITION){
        //non è una foglia
        ret =  ( (val & ((LOCK_NOT_LEAF_MASK) << (pos-1))) != 0);
        return !ret;
    }
    else{
        ret = val & (( (LEAF_FULL_MASK) << (LEAF_START_POSITION-1)) << (5* (pos-(LEAF_START_POSITION))));
        return !ret;
    }
}

//non può essere una macro perchè mi servono gli if e questa viene chiamata all'interno di if
/*
 Questa funziona controlla se il nodo in posizione container_pos è TOTALMENTE occupato (se si tratta di una foglia) oppure se è parzialmente o totalmente occupato se non è una foglia, nella maschera dei nodi rappresentata da val. Ricordo che per la semantica dell'algoritmo una foglia è totalmente occupata se e solo ha il quinto bit settato.
 @param val: il container nel quale il nodo si trova
 @param pos: il container_pos del nodo che vogliamo controllare
 
 */
bool IS_OCCUPIED(unsigned long val, unsigned pos){
    bool ret = false;
    if(pos<LEAF_START_POSITION){
        //non è una foglia
        ret =  ( (val & ((LOCK_NOT_LEAF_MASK) << (pos-1))) != 0);
        return ret;
    }
    else{
        //è una foglia
        ret = val & (( (LOCK_NOT_LEAF_MASK) << (LEAF_START_POSITION-2)) << (5* (pos-(LEAF_START_POSITION-1))));
        return ret;
    }
}



/*
 Funzione di malloc richiesta dall'utente.
 @param pages: memoria richiesta dall'utente
 @return l'indirizzo di memoria del nodo utilizzato per soddisfare la richiesta; NULL in caso di fallimento
 */
node* request_memory(unsigned pages){
    if(pages>MAX_ALLOCABLE_PAGES)
        return NULL;
    
    pages = upper_power_of_two(pages);
    bool restarted;
    unsigned started_at;
    unsigned actual;
    unsigned starting_node = overall_memory_pages / pages; //first node for this level
    unsigned last_node = left_index(&tree[starting_node])-1; //last node for this level
    
#ifdef NUMA
    //inizia a cercare nell'albero relativo al nodo numa in cui sto girando
    int starting_numa_node = numa_node_of_cpu(sched_getcpu());
    int trying_on_numa_node =  starting_numa_node;
try_on_a_new_numa_node_label:
    tree = overall_trees[trying_on_numa_node];
    containers = overall_containers[trying_on_numa_node];
#endif
    
    
    //actual è il posto in cui iniziare a cercare
    
    actual = mypid % number_of_processes;
    
    if(last_node-starting_node!=0)
        actual = actual % (last_node - starting_node);
    else
        actual=0;
    
    actual = starting_node + actual;
    
    
    started_at = actual;
    
    restarted = false;
    
    //quando faccio un giro intero ritorno NULL
    do{
        if(alloc(&tree[actual])==true){
            return &tree[actual];
        }
        
        
        if(failed_at_node==1){ // il buddy è pieno
#ifdef NUMA
            if(allocate_in_remote_node && ((trying_on_numa_node+1)%number_of_numa_nodes) != starting_numa_node){
                trying_on_numa_node = (trying_on_numa_node+1)%number_of_numa_nodes;
                goto try_on_a_new_numa_node_label;
            }
            else
                return NULL;
#else
            return NULL;
#endif
        }
        
        //Questo serve per evitare tutto il sottoalbero in cui ho fallito
        actual=(failed_at_node+1)* (1<<( level(&tree[actual]) - level(&tree[failed_at_node])));
        
        
        if(actual>last_node){ //se ho sforato riparto dal primo utile, se il primo era quello da cui avevo iniziato esco al controllo del while
            actual=starting_node;
            restarted = true;
        }
        
    }while(restarted==false || actual < started_at);
    
#ifdef NUMA
    if(allocate_in_remote_node && ((trying_on_numa_node+1)%number_of_numa_nodes) != starting_numa_node){
        trying_on_numa_node = (trying_on_numa_node+1)%number_of_numa_nodes;
        goto try_on_a_new_numa_node_label;
    }
    else
        return NULL;
#else
    return NULL;
#endif
}

/*
    Questa è una funzione di help per la alloc. Occupa tutti i discendenti del nodo n presenti nello stesso grappolo. NB questa funzione modifica solo new_val; non fa CAS, la modifica deve essere apportata dal chiamante.
    @param n: il nodo (OCCUPATO) a cui occupare i discendenti
    @param new_val: sarebbe  n->container->nodes da modificare
 
 */
unsigned long occupa_discendenti(node* n, unsigned long new_val){
    //se l'ultimo grappolo non ha tutti i nodi che deve avere (tipo ha solo due livelli). QUesto quando andiamo a regime non dovrebbe succedere perchè questa situazione la evitiamo (inutile avere un grappolo monco.. il numero di cas è lo stesso
    if(left_index(n)>=number_of_nodes)
        return new_val;
    if(!IS_LEAF(&left(n))){
        new_val = LOCK_NOT_A_LEAF(new_val, left(n).container_pos);
        new_val = LOCK_NOT_A_LEAF(new_val, right(n).container_pos);
        new_val = occupa_discendenti(&left(n), new_val);
        new_val = occupa_discendenti(&right(n), new_val);
    }
    else{
        new_val = LOCK_A_LEAF(new_val, left(n).container_pos);
        new_val = LOCK_A_LEAF(new_val, right(n).container_pos);
    }
    return new_val;
}

/*
 Prova ad allocare un DATO nodo.
 Con questa allocazione abbiamo che se un generico nodo è occupato, è occupato tutto il suo ramo
 nel grappolo.. cioè se per esempio è allocato uno qualsiasi 1,2,5,10 questi 5 saranno tutti e 5 flaggati come occupati.
 Vengono anche flaggati tutti i figli nello stesso grappolo ma NON i figli nei grappoli sottostanti.
 Side effect: se fallisce subito, prima di chiamare la check_parent la variabile globale failed_at_node assumerà il valore n->pos
 @param n: nodo presunto libero (potrebbe essere diventato occupato concorrentemente)
 @return true se l'allocazione riesce, false altrimenti
 
 */
bool alloc(node* n){
    unsigned long old_val;
    unsigned long new_val;
    node* current;
    node* parent;
    node* root_grappolo;
    do{
        new_val = old_val = n->container->nodes;
        if(!IS_ALLOCABLE(new_val, n->container_pos)){
            failed_at_node = n->pos;
            return false;
        }
        current = n;
        root_grappolo = BUNCHROOT(n);
        parent = &parent(n);
        //marco i genitori in questo grappolo Sicuramente non è una foglia visto che parto da un genitore
        while(current!=root_grappolo){
#ifdef NUMA_AUDIT
            printf("n is %u current is %u and root_grappolo is %u\n", n->pos, current->pos, root_grappolo->pos);
#endif
            new_val = LOCK_NOT_A_LEAF(new_val, parent->container_pos);
            current = parent;
            parent = &parent(current);
        }
        //marco n se è una foglia
        if(IS_LEAF(n)){
            new_val = LOCK_A_LEAF(new_val, n->container_pos);
        }
        //marco n ed i suoi figli se n non è una foglia
        else{
            //marco n
            new_val = LOCK_NOT_A_LEAF(new_val,n->container_pos);
            //marco i suoi discendenti(se questo nodo ha figli). nota che se ha figli gli ha sicuro nello stesso grappolo visto che non è foglia dal controllo precedente
            if(left_index(n)<number_of_nodes)
                new_val = occupa_discendenti(n, new_val);
        }
    }while(!__sync_bool_compare_and_swap(&n->container->nodes, old_val, new_val));
    //Se n appartiene al grappolo della radice
    if(n->container->bunch_root==&ROOT){
        return true;
    }
    else if(check_parent(BUNCHROOT(n))){
        return true;
    }
    else{
        free_node_(n);
        return false;
    }
    
}


/*
 Questa funzione si preoccupa di marcare il bit relativo al sottoalbero del blocco che ho allocato in tutti i suoi antenati.
 Questa funziona ritorna true se e solo se riesce a marcare fino alla radice. La funzione fallisce se e solo se si imbatte in un nodo occupato (ricordando che se un generico nodo è occupato, allora la foglia ad esso relativa (il nodo stesso o un suo discendente) avrà il falore 0x13)
 La funzione si preoccupa inoltre di cancellare l'eventuale bit di coalescing.
 Side effect: se fallisce la variabile globale failed_at_node assume il valore del nodo dove la ricorsione è fallita (TODO: occhio, fallisco sulla foglia, magari il nodo bloccato era il padre, non ho modo di saperlo, rischio di sbattare nuovamente su questo nodo)
 @param n: nodo da cui iniziare la risalita (che è già marcato totalmente o parzialmente). Per costruzione della alloc, n è per forza un nodo radice di un grappolo generico (ma sicuramente non la radice)
 @return true se la funzione riesce a marcare tutti i nodi fino alla radice; false altrimenti.
 
 */
bool check_parent(node* n){
    node* parent = &parent(n);
    node* root_grappolo = parent->container->bunch_root;
    upper_bound = n; //per costruione upper bound sarà quindi la radice dell'ultimo bunch da liberare in caso di fallimento. E' quello precedente al bunch dove lavoriamo adesso perchè se adesso falliamo, non apporteremo le modifiche
    unsigned long new_val, old_val;
    do{
        new_val = old_val = parent->container->nodes;
        if(IS_OCCUPIED(parent->container->nodes, parent->container_pos)){
            failed_at_node = parent->pos;
            return false;
        }
        if(&left(parent)==n){
            new_val = CLEAN_LEFT_COALESCE(new_val, parent->container_pos);
            new_val = OCCUPY_LEFT(new_val, parent->container_pos);
        }else{
            new_val = CLEAN_RIGHT_COALESCE(new_val, parent->container_pos);
            new_val = OCCUPY_RIGHT(new_val, parent->container_pos);
        }
        
        new_val = LOCK_NOT_A_LEAF(new_val, parent(parent).container_pos);
        parent = &parent(parent);
        new_val = LOCK_NOT_A_LEAF(new_val, parent(parent).container_pos);
        new_val = LOCK_NOT_A_LEAF(new_val, root_grappolo->container_pos);
    }while(!__sync_bool_compare_and_swap(&(parent(n).container->nodes),old_val,new_val));
    
    if(root_grappolo==&ROOT)
        return true;
    
    return check_parent(root_grappolo);
}

//MARK: FREE

/*
 Duale della occupa_discendenti
 Questa è una funzione di help per la free. Libera tutti i discendenti del nodo n presenti nello stesso grappolo. NB questa funzione modifica solo new_val; non fa CAS, la modifica deve essere apportata dal chiamante
 @param n: il nodo a cui liberare i discendenti
 @param new_val: sarebbe  n->container->nodes da modificare
 
 */
unsigned long libera_discendenti(node* n, unsigned long new_val){
    
    
    //se l'ultimo grappolo non ha tutti i nodi che deve avere (tipo ha solo due livelli). QUesto quando andiamo a regime non dovrebbe succedere perchè questa situazione la evitiamo (inutile avere un grappolo monco.. il numero di cas è lo stesso
    if(left_index(n)>=number_of_nodes)
        return new_val;
    
    if(!IS_LEAF(&left(n))){
        new_val = UNLOCK_NOT_A_LEAF(new_val, left(n).container_pos);
        new_val = UNLOCK_NOT_A_LEAF(new_val, right(n).container_pos);
        new_val = libera_discendenti(&left(n), new_val);
        new_val = libera_discendenti(&right(n), new_val);
    }
    else{
        new_val = UNLOCK_A_LEAF(new_val,left(n).container_pos);
        new_val = UNLOCK_A_LEAF(new_val,right(n).container_pos);
    }
    return new_val;
}

/*
 Questa funzione fa la free_node da n al nodo rappresentato dalla variabile globale upper_bound.
 Questa funzione potrebbe essere chiamata sia per liberare un nodo occupato, sia per annullare le modifiche apportate da una allocazione che ha fallito (in quel caso upper_bound non è la root ma è il nodo in cui la alloc ha fallito).
 upper_bound è l'ultimo nodo da smarcare ed è, per costruzione, la radice di un grappolo (LO ABBIAMO MARCATO NOI QUINDI LO DOBBIAMO SMARCARE!).
 La funzione è divisa in 3 step.
    1) vengono marcati i grappoli antecedenti come in coalescing (funzione marca)
    2) Viene liberato il nodo n ed il relativo grappolo
    3) vengo smarcati i grappoli antecedenti (funzione smarca)
 @param n è un nodo generico ma per come facciamo qui la allocazione tutto il suo ramo è marcato.
*/
void free_node_(node* n){
#ifdef NUMA
    tree = overall_trees[n->numa_node];
    containers = overall_containers[n->numa_node];
#endif
    bool exit = false;
    if(BUNCHROOT(n) != upper_bound)
        marca(BUNCHROOT(n));
    node* current;
    node* parent;
    unsigned long old_val, new_val;
    //LIBERA TUTTO CIO CHE RIGUARDA IL NODO E I SUOI DISCENDENTI ALL INTERNO DEL GRAPPOLO. ATTENZIONE AI GENITORI ALL'INTERNO DEL GRAPPOLO (p.es. se il fratello
    //è marcato; non smarcare il padre). Nota che fino a smarca non mi interessa di upper_bound
    do{
        exit=false;
        current = n;
        parent = &parent(current);

        old_val = new_val = current->container->nodes;
        while(current!=BUNCHROOT(n)){
            //questo termina il ciclo se il fratello è occupato e setta exit = true
            CHECK_BROTHER(parent,current, new_val);
            
            //qua ci arriviamo solo se il fratello è libero
            new_val = UNLOCK_NOT_A_LEAF(new_val, parent->container_pos);
            current = parent;
            parent = &parent(current);
        }
        if(!IS_LEAF(n) && left_index(n)<=number_of_nodes){ //se non è foglia (nel senso che non è tra le posizione 8-15 e se i figli esistono.
            new_val = libera_discendenti(n,new_val);
        }
        if(IS_LEAF(n))
            new_val = UNLOCK_A_LEAF(new_val, n->container_pos);
        else
            new_val = UNLOCK_NOT_A_LEAF(new_val, n->container_pos);
        
    }while(!__sync_bool_compare_and_swap(&n->container->nodes,old_val, new_val));
    //find_the_bug(2);

    if(BUNCHROOT(n) != upper_bound && !exit)
        smarca_(BUNCHROOT(n));
}

/*
 
 Funzione ausiliaria a free_node_ (in particolare al primo step della free). In questa fase la free vuole marcare tutti i nodi dei sottoalberi interessati dalla free come "coalescing". Qui n è la radice di un grappolo. Bisogna mettere il bit di coalescing al padre (che quindi sarà una foglia con 5 bit). Nota che questa funzione non ha ragione di esistere se il nodo da liberare è nel grappolo di upper_bound.
 @param n è la radice di un grappolo. Bisogna settare in coalescing il padre.
 @return il valore precedente con un singolo nodo marcato come "coalescing"
 
 */
void marca(node* n){
    node* parent = &parent(n);
    unsigned long old_val,new_val;
    do{
        new_val = old_val = parent->container->nodes;
        if (&(left(parent))==n){
            new_val = COALESCE_LEFT(new_val, parent->container_pos);
        }
        else
            new_val = COALESCE_RIGHT(new_val, parent->container_pos);
        
    }while(!__sync_bool_compare_and_swap(&parent->container->nodes, old_val, new_val));
    //find_the_bug(4);
    if(BUNCHROOT(parent)!=upper_bound)
        marca(BUNCHROOT(parent));
}



/*
 Questa funziona leva il bit di coalesce e libera il sottoramo (fino all'upper bound) ove necessario. Nota che potrebbe terminare lasciando alcuni bit di coalescing posti ad 1. Per i dettagli si guardi la documentazione. Upper bound è l'ultimo nodo da smarcare. n è la radice di un grappolo. Va pulito il coalescing bit al padre di n (che è su un altro grappolo). Quando inizio devo essere certo che parent(n) abbia il coalescing bit settato ad 1.
        La funzione termina prima se il genitore del nodo che voglio liberare ha l'altro nodo occupato (sarebbe un errore liberarlo)
 @param n: n è la radice di un grappolo (BISOGNA SMARCARE DAL PADRE)
 
 */
void smarca_(node* n){
    bool exit=false;
    node* current;
    node* parent;
    unsigned long old_val, new_val;
    
    do{
        exit = false;
        current = n;
        parent = &parent(current);
        
        old_val = new_val = parent->container->nodes;
        if(&(left(parent))==current){
            if(!IS_COALESCING_LEFT(new_val, parent->container_pos)) //qualcuno l'ha già pulito
                return;
            new_val = CLEAN_LEFT_COALESCE(new_val, parent ->container_pos); //lo facciamo noi
            new_val = CLEAN_LEFT(new_val, parent -> container_pos);
            //SE il fratello è occupato vai alla CAS
            if(IS_OCCUPIED_RIGHT(new_val, parent -> container_pos))
                continue;
        }
        if(&(right(parent))==current){
            if(!IS_COALESCING_RIGHT(new_val, parent->container_pos)) //qualcuno l'ha già pulito
                return;
            new_val = CLEAN_RIGHT_COALESCE(new_val, parent ->container_pos); //lo facciamo noi
            new_val = CLEAN_RIGHT(new_val, parent -> container_pos);
            //SE il fratello è occupato vai alla CAS
            if(IS_OCCUPIED_LEFT(new_val, parent -> container_pos))
                continue;
        }
        
        
        //ORA VADO A SMARCARE TUTTI GLI ALTRI NODI NON FOGLIA DI QUESTO GRAPPOLO
        current = parent;
        parent = &parent(current);
        while(current!=BUNCHROOT(current)){
            //questo termina il ciclo se il fratello è occupato e setta exit = true
            CHECK_BROTHER(parent, current, new_val);
            
            new_val = UNLOCK_NOT_A_LEAF(new_val, parent->container_pos);
            current = parent;
            parent = &(parent(current));
        }
        
    }while(!__sync_bool_compare_and_swap(&(parent(n).container->nodes), old_val, new_val));
    if(current != upper_bound && !exit) //OCCHIO perchè current è la radice successiva
        smarca_(current); //devo andare su current che sarebbe la radice successiva;
    
}

//MARK: ESECUZIONE

#ifdef SERIAL

void try(){
    unsigned long scelta;
    node* result = NULL;
    
    while(1){
        puts("scrivi 1 per alloc, 2 per free");
        scanf("%lu", &scelta);
        switch(scelta){
            case 1:
                printf("inserisci le pagine che vuoi allocare (MAX %lu)\n", overall_memory_pages);
                scanf("%lu", &scelta);
                result = request_memory(scelta);
                if(result==NULL)
                    puts("allocazione fallita");
                break;
            case 2:
                printf("inserisci l'indice del blocco che vuoi liberare\n"); //quesot non dovrà essere cosi ma stiamo debuggando.. in realtà la free deve essere chiamata senza interazione con l'utente
                scanf("%lu", &scelta);
                free_node(&tree[(int) scelta]);
                break;
            default:
                continue;
    
        }
        puts("Dopo l'esecuzione, l'albero, in ampiezza è:");
        print_in_ampiezza();
    }
}

int main(int argc, char**argv){

    
    puts("main single thread");
    
    if(argc!=2){
        printf("usage: ./a.out <requested memory (in pagine)>\n");
        exit(0);
    }
    number_of_processes = 1;
    unsigned long requested = atol(argv[1]);
    init(requested);
    
    try();
    
    end();
    
    return 0;
}

#else

/*
 
    Con questa funzione faccio un po' di free e di malloc a caso.
 
 */
void parallel_try(){
    srand(getpid());
    int i=0;

    for(i=0;i<10000;i++){
        
        //stampo ogni tanto per controllare che il sistema non si è bloccato
        if(i%500==0)
            printf("(%u) %d\n", mypid, i);
        
        unsigned long scelta = rand();

        //FAI L'ALLOCAZIONE
        if(scelta>=((RAND_MAX/10)*3)){ // 70% di probabilità fai la malloc
            
            //QUA CON SCELTA VIENE DECISO IL NUMERO DELLE PAGINE DA ALLOCARE
            scelta = rand_lim(MAX_ALLOCABLE_PAGES);
            
            if(scelta==0)
                scelta=1;
            
            node* obt;
            obt = request_memory(scelta);
            
            if (obt==NULL){
                //printf("not enough memory\n");
                continue;
            }
            
            taken_list_elem* t = malloc(sizeof(taken_list));
            t->elem = obt;
            t->next = NULL;
            if(takenn->head!=NULL)
                t->next = takenn->head;
            takenn->head = t;
            takenn->number++;
            //find_the_bug(0);
        }
        
        else{//FAI UNA FREE A CASO
            
            if(takenn->number==0){
                continue;
            }
            
            //scelgo il nodo da liberare (nella mia taken list
            scelta = rand_lim((takenn->number)-1);

            if(scelta==0){
                free_node(takenn->head->elem);
                taken_list_elem* die = takenn->head;
                takenn->head = takenn->head->next;
                takenn->number--;
                free(die);
                //find_the_bug(5);
            }
            else{
                taken_list_elem* runner = takenn->head;
                taken_list_elem* chosen;
                int j=0;
                
                for(j=0;j<scelta-1;j++)
                    runner = runner->next;
                
                chosen = runner->next;
                free_node(chosen->elem);
                
                if(runner->next!=NULL)
                    runner->next = runner->next->next;
                else
                    runner->next=NULL;
                
                takenn->number--;
                free(chosen);
                //find_the_bug(1);
            }
            
        }
    }
    //Attendi la finalizzazione di tutti  prima di uscire (per scrivere su file dati coerenti)
    __sync_fetch_and_add(processes_done, 1);
    printf("process done is %d, number of processes is %d\n", *processes_done, number_of_processes);
    while((*processes_done)!=number_of_processes){
        
        sleep(1);
    }
    write_taken();
    if(getpid()==master){
        //find_the_bug_on_new_val(ROOT->container->nodes);
        //print_in_ampiezza();
        write_on_a_file_in_ampiezza();
        write_on_a_file_touched();
    }
    else
        write_on_a_file_in_ampiezza();
    
}

int main(int argc, char**argv){
    
    if(argc!=3){
        printf("usage: ./a.out <number of threads> <requested memory (in pagine)>\n");
        exit(0);
    }
    
#ifdef NUMA
    if(numa_available()<0){
        puts("numa is not available");
        exit(-1);
    }
    number_of_numa_nodes = numa_max_node() + 1;
    //printf("numa nodes are %d\n", number_of_numa_nodes);
    //printf("%d\n", numa_available());
#endif
    
    //Per scrivere solo una volta il risultato finale su file
    master=getpid();
    int i=0;

    number_of_processes=atoi(argv[1]);
    unsigned long requested = atol(argv[2]);
    
    processes_done=mmap(NULL,sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *processes_done=0;
    
    init(requested);
    
    for(i=0; i<log2_(number_of_processes); i++)
        fork();
    
    mypid=getpid();
    
    //per la gestione dei nodi presi dal singolo processo.
    takenn = malloc(sizeof(taken_list));
    takenn->head = NULL;
    takenn->number = 0;
    
    parallel_try();
    
    free(takenn);
    
    return 0;
}

#endif

