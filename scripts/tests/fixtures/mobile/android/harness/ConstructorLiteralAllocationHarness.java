import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConstructorLiteralAllocationHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned constructor literal allocation"); }
    static Core call(int route,Object input,long word,String label) throws Throwable {
        if(route==0)return new Core(input,word,label);
        if(route==1)return ConstructorLiteralAllocationCaller.call(input,word,label);
        if(route==2)try{return Core.class.getConstructor(Object.class,long.class,String.class).newInstance(input,word,label);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,long.class,String.class)).invokeExact(input,word,label);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]),mask=Integer.parseInt(args[2]);
        require(Modifier.isPrivate(Core.class.getDeclaredConstructor(Token.class,Object.class,long.class,String.class).getModifiers()));
        require(Modifier.isFinal(Token.class.getField("mask").getModifiers())&&Token.class.getField("mask").getType()==int.class);
        RuntimeException failure=new IllegalStateException("owned literal constructor failure");Core previous=null;Token previousToken=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object input=mode==7?null:new Object();String label=mode==8?null:new String("owned-"+attempt);
            long word=mode==9?Long.MIN_VALUE:mode==10?Long.MAX_VALUE:mode==11?0x7ff8123456789abcL:mode==12?0:mode==13?-1:0x12345678abcdef01L+attempt;
            Trace.events=0;Trace.failAt=mode>=1&&mode<=6?mode:0;Trace.failure=failure;Trace.token=null;Trace.partial=null;
            Trace.observer=event->{
                if(event==2)require(Trace.token!=null&&Trace.token.first==null&&Trace.token.second==null&&Trace.token.marker==null&&Trace.token.mask==0);
                if(event==4)require(Trace.partial!=null&&Trace.partial.value==null&&Trace.partial.input==null&&Trace.partial.word==0&&Trace.partial.label==null);
                if(event==5)require(Trace.partial.value==Trace.token&&Trace.partial.input==null&&Trace.partial.word==0&&Trace.partial.label==null);
                if(mode==14&&event==2||mode==15&&event==5)throw failure;
            };
            Core result=null;Throwable caught=null;try{result=call(route,input,word,label);}catch(Throwable e){caught=e;}
            long events=0;boolean tokenEntered=false,tokenStored=false,coreEntered=false,valueStored=false,allStored=false;
            for(int event=1;event<=6;event++) {
                if(attempt==1&&event==1)continue;if(attempt==1&&mode==1)break;
                events=events*10+event;
                if(event==2)tokenEntered=true;if(event==3)tokenStored=true;if(event==4)coreEntered=true;if(event==5)valueStored=true;if(event==6)allStored=true;
                if(event==Trace.failAt||mode==14&&event==2||mode==15&&event==5)break;
            }
            require(Trace.events==events);
            if(mode==1)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode>=2&&mode<=6||mode>=14)require(caught==failure);
            else require(caught==null&&result!=null&&result!=previous&&result==Trace.partial);
            require((Trace.token!=null)==tokenEntered&&(Trace.partial!=null)==coreEntered);
            if(tokenEntered){require(Trace.token!=previousToken&&Trace.token.first==null&&Trace.token.second==null&&Trace.token.marker==null&&Trace.token.mask==(tokenStored?mask:0));previousToken=Trace.token;}
            if(coreEntered)require(Trace.partial.value==(valueStored?Trace.token:null)&&Trace.partial.input==(allStored?input:null)&&Trace.partial.word==(allStored?word:0)&&Trace.partial.label==(allStored?label:null));
            if(result!=null)previous=result;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
