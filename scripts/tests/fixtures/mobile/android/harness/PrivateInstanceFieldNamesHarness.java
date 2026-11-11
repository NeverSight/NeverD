import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateInstanceFieldNamesHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned private instance fields"); }
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static long events(int... values) { long result=0; for(int value:values)result=result*100+value; return result; }
    static Callback call(int route, Provider receiver, Callback next, long bits) throws Throwable {
        if(route==0)return Client.call(receiver,next,bits);
        if(route==1)return PrivateInstanceFieldNamesCaller.call(receiver,next,bits);
        if(route==2)try{return (Callback)Provider.class.getMethod("exchange",Callback.class,long.class).invoke(receiver,next,bits);}catch(InvocationTargetException error){throw error.getCause();}
        return (Callback)MethodHandles.publicLookup().findVirtual(Provider.class,"exchange",MethodType.methodType(Callback.class,Callback.class,long.class)).invokeExact(receiver,next,bits);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            Object value=mode==4?null:new Object();
            Object anchor=mode==1?null:mode==18?value:new Object(),otherAnchor=new Object();
            Object output=mode==19?null:new Object();
            Throwable failure=(mode==8||mode==11||mode==14)?new Exception("owned checked"):
                (mode==10||mode==13||mode==15)?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            int[] calls={0};
            Callback<Object,Object> initial=argument->{require(argument==value);Trace.hit(10);calls[0]++;return value;};
            Callback<Object,Object> old=mode==2?null:initial;
            Callback<Object,Object> other=argument->{throw new AssertionError("owned wrong receiver");};
            long bits=17+attempt,otherBits=-41-attempt,nextBits=mode==5?Long.MIN_VALUE:mode==6?Long.MAX_VALUE:91+attempt;
            Trace.events=0;Trace.seen=null;Trace.seenInput=null;Trace.created=null;
            Trace.action=()->PrivateInstanceFieldNamesHarness.<RuntimeException>raise(failure);
            Trace.failAt=mode==14?2:mode==15?4:0;
            Provider<Object> first=null;Throwable constructorFailure=null;
            try {first=new Provider<Object>(old,anchor,bits);}catch(Throwable error){constructorFailure=error;}
            if(mode==14||mode==15) {
                require(constructorFailure==failure && Trace.created instanceof Provider);
                Provider partial=(Provider)Trace.created;
                require(partial.anchor()==anchor && partial.peek()==(mode==14?null:old) && partial.bits()==(mode==14?0:bits));
                require(Trace.events==(mode==14?events(1,2):events(1,2,3,4)));
                continue;
            }
            require(constructorFailure==null && Trace.created==first && Trace.events==events(1,2,3,4));
            require(first.anchor()==anchor && first.peek()==old && first.bits()==bits);
            Trace.events=0;
            Provider<Object> second=new Provider<Object>(other,otherAnchor,otherBits);
            require(Trace.created==second && Trace.events==events(1,2,3,4));
            require(first.anchor()==anchor && first.peek()==old && first.bits()==bits);
            Callback<Object,Object> replacement=argument->{
                require(argument==value);Trace.seenInput=argument;Trace.hit(10);calls[0]++;
                if(mode>=11&&mode<=13)PrivateInstanceFieldNamesHarness.<RuntimeException>raise(failure);
                if(mode==16){require(second.exchange(initial,nextBits)==other);Trace.hit(11);}
                return output;
            };
            Callback<Object,Object> next=mode==3?null:mode==17?initial:replacement;
            Trace.events=0;Trace.seen=null;Trace.seenInput=null;
            Trace.failAt=mode==8?5:mode==9?7:mode==10?8:0;
            Callback previous=null;Object result=null;Throwable caught=null;
            try {previous=call(route,mode==7?null:first,next,nextBits);result=first.invoke(value);}catch(Throwable error){caught=error;}
            if(mode==7) {
                require(caught instanceof NullPointerException && Trace.events==0 && calls[0]==0);
                require(first.peek()==old && first.bits()==bits && Trace.seen==null);
            } else if(mode>=8&&mode<=10) {
                require(caught==failure && calls[0]==0);
                require(first.peek()==(mode==8?old:next) && first.bits()==(mode==10?nextBits:bits));
                require(Trace.seen==(mode==8?null:old));
                require(Trace.events==(mode==8?events(5):mode==9?events(5,6,7):events(5,6,7,8)));
            } else {
                require(previous==old && first.peek()==next && first.bits()==nextBits);
                require(calls[0]==(mode==3?0:1));
                if(mode==3)require(caught instanceof NullPointerException);
                else if(mode>=11&&mode<=13)require(caught==failure);
                else require(caught==null && result==(mode==17?value:output));
                require(Trace.seen==(mode==16?other:old));
                require(Trace.seenInput==(mode==3||mode==17?null:value));
                require(Trace.events==(mode==3?events(5,6,7,8,9):mode==16?events(5,6,7,8,9,10,5,6,7,8,11):events(5,6,7,8,9,10)));
            }
            require(first.anchor()==anchor && second.anchor()==otherAnchor);
            require(second.peek()==(mode==16?initial:other) && second.bits()==(mode==16?nextBits:otherBits));
        }
        System.out.println("attempts=2,private-instance-fields=matched");
    }
}
