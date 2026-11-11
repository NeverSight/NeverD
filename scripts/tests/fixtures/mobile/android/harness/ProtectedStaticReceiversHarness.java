import api.Bound;
import parent.Base;
import fixture.Child;
import java.lang.reflect.*;
public final class ProtectedStaticReceiversHarness {
    static final class First implements Bound {}
    static final class Second implements Bound {}
    static final class Derived extends Child {}
    static void require(boolean condition){if(!condition)throw new AssertionError("owned protected static receiver");}
    static void metadata()throws Exception{
        Method target=Base.class.getDeclaredMethod("choose",Bound.class);TypeVariable<?>[] own=target.getTypeParameters();
        require(Modifier.isProtected(target.getModifiers()) && Modifier.isFinal(target.getModifiers()) && !Modifier.isStatic(target.getModifiers()));
        require(own.length==1 && own[0].getGenericDeclaration().equals(target) && own[0].getBounds().length==1 && own[0].getBounds()[0]==Bound.class);
        require(target.getGenericParameterTypes()[0].equals(own[0]) && target.getGenericReturnType().equals(own[0]));
        for(String name:new String[]{"apply","copied"}){
            Method call=Child.class.getDeclaredMethod(name,Child.class,Bound.class);require(Modifier.isStatic(call.getModifiers()) && call.getTypeParameters().length==0 && call.getReturnType()==Bound.class);
        }
    }
    public static void main(String[] args)throws Exception{
        int route=Integer.parseInt(args[0]),alias=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);metadata();Throwable previous=null;
        for(int attempt=0;attempt<2;attempt++){
            Child first=new Child(),second=new Child(),derived=new Derived();Child receiver=alias==0?first:alias==1?second:derived;
            Bound input=new First(),expected=new Second();if(mode==1)expected=input;if(mode==2)expected=null;if(mode==3)input=null;
            if(mode==7){input=receiver;expected=first==receiver?second:first;}if(mode==9)input=new Second();
            RuntimeException failure=mode==8?null:new IllegalArgumentException("owned exact failure");receiver.replacement=expected;receiver.failure=failure;receiver.fail=mode==4 || mode==8?1:mode==5?2:0;
            Child selected=mode==6?null:receiver;Bound result=null;Throwable seen=null;
            try{result=route==0?Child.apply(selected,input):Child.copied(selected,input);}catch(Throwable error){seen=error;}
            int calls=mode==6?0:1;Bound observed=mode==6 || mode==4 || mode==8?null:input;
            require(receiver.calls==calls && receiver.seen==observed && receiver.replacement==expected && receiver.failure==failure);
            for(Child other:new Child[]{first,second,derived})if(other!=receiver)require(other.calls==0 && other.seen==null && other.replacement==null && other.failure==null);
            if(mode==4 || mode==5)require(seen==failure);
            else if(mode==6 || mode==8)require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous);
            else require(seen==null && result==expected);
            previous=seen;
        }
        System.out.println("attempts=2,protected-static-receiver=matched");
    }
}
