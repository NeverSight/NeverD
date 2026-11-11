import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProducerPredicateCompletionHarness {
    static long events;
    static int failAt,reads,predicates,accepts,contexts,booleanAccepts;
    static Object item,test,token;
    static RuntimeException failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned producer predicate completion");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Read implements Producer {public Object next(){reads++;hit(2);return item;}}
    static final class Test implements Predicate {public Object apply(Object value,Object completion){predicates++;require(value==item && completion==Provider.booleanCompletion);hit(3);((Completion)completion).accept(Boolean.TRUE);return test;}}
    static final class Finish implements Completion {
        final boolean bool;Finish(boolean bool){this.bool=bool;}
        public Object context(){contexts++;hit(1);return token;}
        public void accept(Object value){if(bool){booleanAccepts++;require(value==Boolean.TRUE);hit(4);}else{accepts++;require(value==item);hit(5);}}
    }
    static Object call(int route,Producer producer,Predicate predicate,Completion completion) throws Throwable {
        if(route==0)return Client.call(producer,predicate,completion);
        if(route==1)return ProducerPredicateCompletionCaller.call(producer,predicate,completion);
        if(route==2){try{return Provider.class.getMethod("first",Producer.class,Predicate.class,Completion.class).invoke(null,producer,predicate,completion);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Provider.class,"first",MethodType.methodType(Object.class,Producer.class,Predicate.class,Completion.class)).invokeExact(producer,predicate,completion);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method m=Provider.class.getMethod("first",Producer.class,Predicate.class,Completion.class);TypeVariable<?> t=m.getTypeParameters()[0];require(m.getTypeParameters().length==1 && t.getBounds()[0]==Object.class && m.getGenericReturnType()==Object.class);
        ParameterizedType p=(ParameterizedType)m.getGenericParameterTypes()[0],q=(ParameterizedType)m.getGenericParameterTypes()[1],r=(ParameterizedType)m.getGenericParameterTypes()[2];
        require(((WildcardType)p.getActualTypeArguments()[0]).getUpperBounds()[0].equals(t));
        require(((WildcardType)q.getActualTypeArguments()[0]).getLowerBounds()[0].equals(t) && ((WildcardType)r.getActualTypeArguments()[0]).getLowerBounds()[0].equals(t));
        ParameterizedType nested=(ParameterizedType)((WildcardType)q.getActualTypeArguments()[1]).getLowerBounds()[0];require(nested.getRawType()==Completion.class && ((WildcardType)nested.getActualTypeArguments()[0]).getLowerBounds()[0]==Boolean.class);
        require(((WildcardType)q.getActualTypeArguments()[2]).getUpperBounds()[0]==Object.class);
        int[] fail={0,0,0,0,0,1,2,3,4,5,0,0,0,0,0};
        for(int attempt=0;attempt<2;attempt++){
            events=0;contexts=reads=predicates=accepts=booleanAccepts=0;failAt=fail[mode];failure=new RuntimeException("owned predicate failure");
            item=mode==1?null:mode==12?new Long(Long.MIN_VALUE+attempt):mode==13?new Long(Long.MAX_VALUE-attempt):new Object();
            test=mode==10?null:mode==11?new Object():mode==4?Boolean.FALSE:Boolean.TRUE;
            token=new Object();Provider.miss=new Object();Provider.context=Provider.seenItem=Provider.seenPredicate=null;Provider.booleanCompletion=new Finish(true);
            Producer producer=mode==2?null:new Read();Predicate predicate=mode==3?null:new Test();Completion completion=mode==14?null:new Finish(false);
                        Object result=null;Throwable caught=null;try{result=call(route,producer,predicate,completion);}catch(Throwable p2){caught=p2;}
            if(mode>=5&&mode<=9)require(caught==failure);else if(mode==2||mode==3||mode==10||mode==14)require(caught instanceof NullPointerException);else if(mode==11)require(caught instanceof ClassCastException);else require(caught==null && result==(mode==4?Provider.miss:item));
            long expected=mode==5?1:mode==2||mode==6?12:mode==3||mode==7?123:mode==8||mode==10||mode==11||mode==4?1234:12345;
            if(mode==2)expected=1;if(mode==3)expected=12;if(mode==14)expected=0;
            require(events==expected && contexts==(mode==14?0:1) && reads==((mode==2||mode==5||mode==14)?0:1) && predicates==((mode==2||mode==3||mode==5||mode==6||mode==14)?0:1));
            require(booleanAccepts==((mode==2||mode==3||mode==5||mode==6||mode==7||mode==14)?0:1));
            require(accepts==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==7||mode==8||mode==10||mode==11||mode==14)?0:1));
            require(Provider.context==(mode==5||mode==14?null:token));
            require(Provider.seenItem==((mode==2||mode==5||mode==6||mode==14)?null:item));
            require(Provider.seenPredicate==((mode==2||mode==3||mode==5||mode==6||mode==7||mode==8||mode==14)?null:test));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
