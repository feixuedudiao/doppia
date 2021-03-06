function [crop, meany] = get_crop_and_mean(folder, resultfolder, range, show)
    dirs=get_sorted_dirnames(folder);
    colors = varycolor(17);
    lims=[3e-3 1e1 .025 1];
    if show
        figure(123); hold on; grid;
         axis( lims ); lgdLoc='sw'; plot([1 1],[eps 1],'-k','HandleVisibility','off');
    set(gca,'XScale','log','YScale','log','YTick',[0 .05 .1:.1:1]);
    set(gca,'XMinorGrid','off','XMinorTic','off');
    set(gca,'YMinorGrid','off','YMinorTic','off');
    fnt={ 'FontSize',12 };
    set(gcf, 'DefaultAxesColorOrder', colors);
    str='';
    if(~isempty(str)), title(['ROC for ' str],fnt{:}); end
    
    xlabel('false positives per image',fnt{:});
    ylabel('miss rate',fnt{:});
    end

    for i=1:numel(dirs) 
        if numel(dirs) ==17
            name{i}=sprintf('%.1f %% occl.',(100*(i-1)*4/128) );    
        elseif numel(dirs) ==9
            name{i}=sprintf('%.1f %% occl.',(100*(i-1)*4/64) );  
        else
            name{i}=dirs{i};
        end
        
        foldername = ['set01_' dirs{i}];
        
        %name{i}=['occl ' int2str((i-1)*4) 'px'];
        matfilename =[dirs{i} 'Ours-wip.mat'];
        matfilename = fullfile(resultfolder, foldername, matfilename)
        if ~exist(matfilename, 'file')
            continue;
        end
        load(matfilename);
        xs = xy(:,1);
        ys = xy(:,2);
        m = xs > range(1) & xs < range(2);
        meany(i) = 1-mean(ys(m));
        if numel(dirs) ==17
            crop(i) = (128 - (i-1)*4)/128;
        elseif numel(dirs) ==9
            crop(i) = (64 - (i-1)*4)/64; 
        else
            crop(i) = i;
        end
        
        if show
            plot(xs(m), ys(m), 'Color', colors(i,:), 'linewidth',3 );
            
        end
    end
    %p = patch([0.1,1,1,0.1,0.1],[0.005,0.005,1,1, 0.005], 'y', 'FaceAlpha', 0.01,'EdgeColor','none');
    legend(name,'FontSize',15)
   crop = 1-crop;
  
end