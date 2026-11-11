import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TaggedCompletionCallbackHarness {
    static long events;
    static int failAt,contexts,calls,accepts;
    static Object token,seen,accepted,contextToken;
    static RuntimeException failure;
    static Completion expectedCompletion;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned tagged completion callback");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Invoke implements Callback {
        public Object apply(Object input){calls++;require(input==expectedCompletion);seen=input;hit(2);((Completion)input).accept(token);return token;}
    }
    static final class Finish implements Completion {
        public Object context(){contexts++;hit(1);return contextToken;}
        public void accept(Object value){accepts++;accepted=value;require(value==token);hit(3);}
    }
    static Object call(int route,Policy policy,Tag tag,Callback callback,Completion completion) throws Throwable {
        if(route==0)return Client.call(policy,tag,callback,completion);
        if(route==1)return TaggedCompletionCallbackCaller.call(policy,tag,callback,completion);
        if(route==2){try{return Policy.class.getMethod("work",Tag.class,Callback.class,Completion.class).invoke(policy,tag,callback,completion);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Object)MethodHandles.publicLookup().findVirtual(Policy.class,"work",MethodType.methodType(Object.class,Tag.class,Callback.class,Completion.class)).invokeExact(policy,tag,callback,completion);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int policyMode=mode==11?1:0;
        Method method=Policy.class.getDeclaredMethod("work",Tag.class,Callback.class,Completion.class);
        require(method.getTypeParameters().length==1 && method.getTypeParameters()[0].getBounds()[0]==Object.class && method.getGenericReturnType()==Object.class);
        ParameterizedType cb=(ParameterizedType)method.getGenericParameterTypes()[1],out=(ParameterizedType)method.getGenericParameterTypes()[2];
        require(cb.getRawType()==Callback.class && out.getRawType()==Completion.class);
        ParameterizedType nested=(ParameterizedType)((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0];
        require(nested.getRawType()==Completion.class && ((WildcardType)nested.getActualTypeArguments()[0]).getLowerBounds()[0].equals(method.getTypeParameters()[0]));
        require(((WildcardType)cb.getActualTypeArguments()[1]).getUpperBounds()[0]==Object.class);
        require(((WildcardType)out.getActualTypeArguments()[0]).getLowerBounds()[0].equals(method.getTypeParameters()[0]));
        int[] failures={0,0,0,0,1,2,3,0,0,2,3,0,0,0};
        for(int attempt=0;attempt<2;attempt++){
            token=mode==1?null:mode==12?new Long(91+attempt):new Object();
            contextToken=new Object();failure=new RuntimeException("owned callback failure");
            events=0;contexts=calls=accepts=0;seen=accepted=null;failAt=failures[mode];
            Provider.context=Provider.seenResult=null;Provider.saved=null;Provider.savedCompletion=null;
            Provider provider=new Provider();Tag tag=mode==1?null:new Tag(0x1234567887654321L+attempt);
            Provider.sentinel=mode==13?null:new Object();Provider.deferred=mode>=7 && mode<=10;
            Callback callback=mode==3||mode==8?null:new Invoke();Completion completion=mode==2?null:new Finish();
            expectedCompletion=completion;Object result=null;Throwable caught=null;
            try{result=call(route,policyMode==0?provider:null,tag,callback,completion);}catch(Throwable p){caught=p;}
            boolean initialFail=mode==2||mode==3||mode==4||mode==5||mode==6||mode==11;
            if(mode==2||mode==3||mode==11)require(caught instanceof NullPointerException);
            else if(initialFail)require(caught==failure);else require(caught==null);
            if(Provider.deferred){
                require(result==Provider.sentinel && Provider.saved==callback && Provider.savedCompletion==completion && events==1 && calls==0 && accepts==0);
                require(Provider.context==contextToken && Provider.seenResult==null);
                try{result=Provider.resume();}catch(Throwable p){caught=p;}
                if(mode==8)require(caught instanceof NullPointerException);
                else if(mode==9||mode==10)require(caught==failure);else require(caught==null && result==token);
            }else if(!initialFail)require(result==token);
            long expected=mode==2||mode==11?0:mode==3||mode==4||mode==8?1:mode==5||mode==9?12:123;
            require(events==expected && contexts==((mode==2||mode==11)?0:1) && calls==((mode==2||mode==3||mode==4||mode==8||mode==11)?0:1));
            require(accepts==((mode==2||mode==3||mode==4||mode==5||mode==8||mode==9||mode==11)?0:1));
            require(Provider.context==((mode==2||mode==4||mode==11)?null:contextToken));
            require(Provider.seenResult==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==8||mode==9||mode==10||mode==11)?null:token));
            if(accepts!=0)require(accepted==token);
            require(provider.visits==(mode==11?0:1) && provider.seenTag==(mode==11?null:tag));
            require(tag==null || tag.value==0x1234567887654321L+attempt);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
